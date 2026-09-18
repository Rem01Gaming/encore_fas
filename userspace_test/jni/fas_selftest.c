#define _GNU_SOURCE
#include "fas_selftest.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fas_client.h"
#include "fas_elf.h"

#define MAX_EVENTS 1024
#define MS 1000000ull

/**
 * @brief The function that the module probes.
 *
 * The test calls it like a game calls Surface::queueBuffer. The attributes keep
 * the function out of inlining and keep its symbol in the dynamic symbol table.
 */
__attribute__((noinline, used, visibility("default"))) void fas_probe_frame(void) {
    __asm__ volatile("" ::: "memory");
}

struct scenario {
    int fd;
    int ctx;
    uint64_t next;
    struct fas_event ev[MAX_EVENTS];
    size_t n;
    int failed;
};

static uint32_t g_next_seq;
static int g_seq_checked;

static uint64_t now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t t) {
    struct timespec ts = {.tv_sec = (time_t)(t / 1000000000ull), .tv_nsec = (long)(t % 1000000000ull)};

    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

/**
 * @brief Sends frames at a fixed rate.
 *
 * @param s The scenario. The function keeps the schedule in s->next.
 * @param fps The frame rate.
 * @param seconds The duration.
 */
static void frames(struct scenario *s, unsigned fps, double seconds) {
    uint64_t period = 1000000000ull / fps;
    uint64_t end = s->next + (uint64_t)(seconds * 1e9);

    while (s->next < end) {
        s->next += period;
        sleep_until(s->next);
        fas_probe_frame();
    }
}

/**
 * @brief Delays the next frame. The next frames stay late (a shift hitch).
 *
 * @param s The scenario.
 * @param ms The delay in milliseconds.
 */
static void delay_ms(struct scenario *s, unsigned ms) {
    s->next += (uint64_t)ms * MS;
}

/**
 * @brief Stops the frames for a time.
 *
 * @param s The scenario.
 * @param seconds The length of the gap.
 */
static void silence(struct scenario *s, double seconds) {
    s->next += (uint64_t)(seconds * 1e9);
    sleep_until(s->next);
}

/**
 * @brief Reads all events that the queue holds and checks the sequence numbers.
 *
 * @param s The scenario. The function appends to s->ev.
 */
static void drain(struct scenario *s) {
    for (;;) {
        ssize_t n = fas_client_read_events(s->fd, s->ev + s->n, MAX_EVENTS - s->n);

        if (n <= 0)
            break;

        for (ssize_t i = 0; i < n; i++) {
            const struct fas_event *e = &s->ev[s->n + (size_t)i];

            if (e->ctx_id != s->ctx)
                continue;
            if (g_seq_checked && e->seq != g_next_seq) {
                printf("    note: sequence gap, expected %u got %u\n", g_next_seq, e->seq);
                s->failed++;
            }
            g_next_seq = e->seq + 1;
            g_seq_checked = 1;
        }
        s->n += (size_t)n;
    }
}

static int count_type(const struct scenario *s, uint32_t type) {
    int c = 0;

    for (size_t i = 0; i < s->n; i++)
        if (s->ev[i].type == type)
            c++;
    return c;
}

static int first_index(const struct scenario *s, uint32_t type) {
    for (size_t i = 0; i < s->n; i++)
        if (s->ev[i].type == type)
            return (int)i;
    return -1;
}

static int switch_fps(const struct scenario *s, int nth) {
    for (size_t i = 0; i < s->n; i++)
        if (s->ev[i].type == FAS_EVENT_RATE_SWITCH && nth-- == 0)
            return (int)s->ev[i].fps;
    return -1;
}

/**
 * @brief Records the result of one check.
 *
 * @param s The scenario.
 * @param ok The result of the check.
 * @param fmt The text of the check, in printf format.
 */
static void check(struct scenario *s, int ok, const char *fmt, ...) {
    va_list ap;

    if (ok)
        return;

    s->failed++;
    printf("    FAIL: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/**
 * @brief Starts a scenario. The function loads a new target list, which resets the detector.
 *
 * @param s The scenario.
 * @param fps The target list.
 * @param count The number of targets.
 * @param vsync_hz The display refresh rate.
 * @param lock_down Non-zero forbids a switch to a slower target.
 */
static void begin(struct scenario *s, const uint32_t *fps, uint32_t count, uint32_t vsync_hz, int lock_down) {
    struct fas_config cfg;

    fas_client_config_init(&cfg, fps, count, vsync_hz, lock_down);
    if (fas_client_set_config(s->fd, s->ctx, &cfg) != 0) {
        perror("    set_config");
        s->failed++;
    }
    s->n = 0;
    drain(s);
    s->n = 0;
    s->next = now_ns();
}

static void sc_healthy(struct scenario *s) {
    const uint32_t f[] = {60};
    struct fas_state st;

    begin(s, f, 1, 60, 0);
    frames(s, 60, 3.0);
    drain(s);

    check(s, count_type(s, FAS_EVENT_RATE_SWITCH) == 1 && switch_fps(s, 0) == 60, "acquire 60 fps");
    check(s, count_type(s, FAS_EVENT_DEGRADED) == 0, "no degraded state");
    check(s, count_type(s, FAS_EVENT_BIG_JANK) == 0, "no big hitch");
    check(s, count_type(s, FAS_EVENT_SMALL_JANK) <= 2, "at most 2 small hitches, got %d", count_type(s, FAS_EVENT_SMALL_JANK));
    check(s, fas_client_get_state(s->fd, s->ctx, &st) == 0 && !(st.flags & FAS_STATE_ACQUIRING) && st.fps == 60, "state after acquisition");
}

static void sc_hitch(struct scenario *s) {
    const uint32_t f[] = {60};

    begin(s, f, 1, 60, 0);
    frames(s, 60, 1.0);
    for (int i = 0; i < 5; i++) {
        frames(s, 60, 0.5);
        delay_ms(s, 25);
    }
    frames(s, 60, 0.5);
    drain(s);

    int hitches = count_type(s, FAS_EVENT_SMALL_JANK) + count_type(s, FAS_EVENT_BIG_JANK);
    int wd = 0;

    for (size_t i = 0; i < s->n; i++)
        if ((s->ev[i].type == FAS_EVENT_SMALL_JANK || s->ev[i].type == FAS_EVENT_BIG_JANK) && (s->ev[i].flags & FAS_EVF_WATCHDOG))
            wd++;

    check(s, hitches >= 4, "5 hitches sent, %d reported", hitches);
    check(s, count_type(s, FAS_EVENT_BOOST_SOFT) >= 4, "soft watchdog events: %d", count_type(s, FAS_EVENT_BOOST_SOFT));
    check(s, wd >= 3, "hitch events with the watchdog flag: %d", wd);
}

static void sc_deficit(struct scenario *s) {
    const uint32_t f[] = {60};
    struct fas_state st;

    begin(s, f, 1, 60, 0);
    frames(s, 60, 1.0);
    frames(s, 45, 1.5);
    drain(s);
    check(s, count_type(s, FAS_EVENT_DEGRADED) == 1, "degraded events: %d", count_type(s, FAS_EVENT_DEGRADED));
    check(s, fas_client_get_state(s->fd, s->ctx, &st) == 0 && (st.flags & FAS_STATE_DEGRADED), "degraded flag in the state");

    frames(s, 60, 2.5);
    drain(s);
    int d = first_index(s, FAS_EVENT_DEGRADED);
    int r = first_index(s, FAS_EVENT_RECOVERED);

    check(s, r >= 0 && d >= 0 && d < r, "recovered after degraded");
}

static void sc_pause(struct scenario *s) {
    const uint32_t f[] = {60};

    begin(s, f, 1, 60, 0);
    frames(s, 60, 1.0);
    silence(s, 1.5);
    frames(s, 60, 0.5);
    drain(s);

    int p = first_index(s, FAS_EVENT_PAUSED);
    int r = first_index(s, FAS_EVENT_RESUMED);

    check(s, count_type(s, FAS_EVENT_BOOST_SOFT) >= 1 && count_type(s, FAS_EVENT_BOOST_HARD) >= 1, "soft and hard events");
    check(s, p >= 0 && r > p, "paused then resumed");
    check(s, count_type(s, FAS_EVENT_BIG_JANK) == 0, "no hitch after the pause");
}

static void sc_rates(struct scenario *s) {
    const uint32_t f[] = {30, 60};

    begin(s, f, 2, 60, 0);
    frames(s, 60, 1.0);
    frames(s, 30, 3.0);
    frames(s, 60, 2.0);
    drain(s);
    check(s, switch_fps(s, 0) == 60 && switch_fps(s, 1) == 30 && switch_fps(s, 2) == 60, "rate sequence 60, 30, 60 (got %d, %d, %d)", switch_fps(s, 0),
          switch_fps(s, 1), switch_fps(s, 2));

    begin(s, f, 2, 60, 1);
    frames(s, 60, 1.0);
    frames(s, 30, 2.0);
    drain(s);
    check(s, count_type(s, FAS_EVENT_RATE_SWITCH) == 1, "lock stops the switch to 30");
    check(s, count_type(s, FAS_EVENT_DEGRADED) == 1, "lock keeps the degraded state");
}

/**
 * @brief Tests the error paths of the interface and the cleanup of a dead process.
 *
 * @param s The scenario.
 * @param path The path of this executable.
 * @param off The file offset of fas_probe_frame.
 * @param quick Non-zero skips the wait for the cleanup.
 */
static void sc_api(struct scenario *s, const char *path, uint64_t off, int quick) {
    struct fas_config bad, good;
    struct fas_listener_list list;
    struct fas_state st;
    const uint32_t one[] = {60}, dup[] = {60, 60};
    int ctx2 = -1;
    pid_t child;

    fas_client_config_init(&good, one, 1, 0, 0);
    fas_client_config_init(&bad, dup, 2, 0, 0);
    errno = 0;
    check(s, fas_client_set_config(s->fd, s->ctx, &bad) == -1 && errno == EINVAL, "duplicate rates give EINVAL");
    fas_client_config_init(&bad, one, 0, 0, 0);
    errno = 0;
    check(s, fas_client_set_config(s->fd, s->ctx, &bad) == -1 && errno == EINVAL, "empty list gives EINVAL");
    errno = 0;
    check(s, fas_client_register(s->fd, getpid(), path, off, &good, NULL) == -1 && errno == EEXIST, "second listener on one process gives EEXIST");
    errno = 0;
    check(s, fas_client_get_state(s->fd, 0x7ffffff0, &st) == -1 && errno == ENOENT, "state of an unknown listener gives ENOENT");
    errno = 0;
    check(s, fas_client_remove(s->fd, 0x7ffffff0) == -1 && errno == ENOENT, "remove of an unknown listener gives ENOENT");

    child = fork();
    if (child == 0) {
        pause();
        _exit(0);
    }
    usleep(50 * 1000);

    errno = 0;
    check(s, fas_client_register(s->fd, child, "/nonexistent/file", off, &good, &ctx2) == -1 && errno == ENOENT, "missing file gives ENOENT");
    errno = 0;
    check(s, fas_client_register(s->fd, child, path, off + 1, &good, &ctx2) == -1 && errno == EINVAL, "unaligned offset gives EINVAL");
    errno = 0;
    check(s, fas_client_register(s->fd, child, "/proc/self/maps", 0, &good, &ctx2) == -1 && errno == EINVAL, "not a regular file gives EINVAL");
    errno = 0;
    check(s, fas_client_register(s->fd, 0x3ffffff0, path, off, &good, &ctx2) == -1 && errno == ESRCH, "unknown process gives ESRCH");

    check(s, fas_client_register(s->fd, child, path, off, &good, &ctx2) == 0 && ctx2 > 0, "register on a child process");
    check(s, fas_client_list(s->fd, &list) == 0 && list.count == 2, "list holds 2 listeners, got %u", list.count);

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    if (!quick) {
        int gone = 0;

        for (int i = 0; i < 100 && !gone; i++) {
            usleep(100 * 1000);
            gone = fas_client_list(s->fd, &list) == 0 && list.count == 1;
        }
        check(s, gone, "the module removes the listener of a dead process");
    } else {
        fas_client_remove(s->fd, ctx2);
    }
}

/**
 * @brief Tests that a listener sees frames of its own process only.
 *
 * A second listener watches an idle child process. The uprobe core calls the
 * handler of both listeners for a frame of the parent. The second listener
 * must ignore that frame.
 *
 * @param s The scenario.
 * @param path The path of this executable.
 * @param off The file offset of fas_probe_frame.
 */
static void sc_isolation(struct scenario *s, const char *path, uint64_t off) {
    const uint32_t f[] = {60};
    struct fas_config cfg;
    struct fas_state st;
    int ctx2 = -1;
    pid_t child = fork();

    if (child == 0) {
        pause();
        _exit(0);
    }
    usleep(50 * 1000);

    fas_client_config_init(&cfg, f, 1, 60, 0);
    check(s, fas_client_register(s->fd, child, path, off, &cfg, &ctx2) == 0, "register on the child");

    begin(s, f, 1, 60, 0);
    frames(s, 60, 2.0);
    drain(s);

    int leaked = 0;

    for (size_t i = 0; i < s->n; i++)
        leaked += s->ev[i].ctx_id == ctx2;
    check(s, count_type(s, FAS_EVENT_RATE_SWITCH) == 1, "the parent listener works");
    check(s, leaked == 0, "the idle listener got %d events from another process", leaked);
    check(s, fas_client_get_state(s->fd, ctx2, &st) == 0 && (st.flags & FAS_STATE_ACQUIRING) && st.seq == 0, "the idle listener saw no frame");

    fas_client_remove(s->fd, ctx2);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
}

struct stress {
    volatile int stop;
    uint64_t seed;
};

static void *frame_thread(void *arg) {
    struct stress *st = arg;
    uint64_t next = now_ns();

    while (!st->stop) {
        next += 4 * MS;
        sleep_until(next);
        fas_probe_frame();
    }
    return NULL;
}

/**
 * @brief Runs frames on two threads while the main thread changes the state.
 *
 * The test has no exact expectation. It looks for a hang, an error code that
 * the interface must not return, and a kernel warning (the caller checks the
 * kernel log).
 *
 * @param s The scenario.
 * @param path The path of this executable.
 * @param off The file offset of fas_probe_frame.
 */
static void sc_stress(struct scenario *s, const char *path, uint64_t off) {
    const uint32_t f1[] = {60}, f2[] = {30, 60, 120};
    struct fas_config c1, c2;
    struct fas_state st;
    struct fas_listener_list list;
    struct stress ctl = {0};
    pthread_t th[2];
    int bad = 0, cycles = 0;
    pid_t child = fork();

    if (child == 0) {
        pause();
        _exit(0);
    }
    usleep(50 * 1000);

    fas_client_config_init(&c1, f1, 1, 60, 0);
    fas_client_config_init(&c2, f2, 3, 120, 1);
    begin(s, f1, 1, 60, 0);

    pthread_create(&th[0], NULL, frame_thread, &ctl);
    pthread_create(&th[1], NULL, frame_thread, &ctl);

    uint64_t end = now_ns() + 3000 * MS;

    while (now_ns() < end) {
        int ctx2 = -1;

        bad += fas_client_set_config(s->fd, s->ctx, (cycles & 1) ? &c1 : &c2) != 0;
        bad += fas_client_get_state(s->fd, s->ctx, &st) != 0;
        bad += fas_client_list(s->fd, &list) != 0;
        bad += fas_client_register(s->fd, child, path, off, &c1, &ctx2) != 0;
        bad += fas_client_remove(s->fd, ctx2) != 0;
        drain(s);
        s->n = 0;
        cycles++;
    }

    ctl.stop = 1;
    pthread_join(th[0], NULL);
    pthread_join(th[1], NULL);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);

    check(s, bad == 0, "%d interface calls failed in %d cycles", bad, cycles);
    check(s, fas_client_get_state(s->fd, s->ctx, &st) == 0, "state after the stress");
    printf("    %d cycles\n", cycles);
    g_seq_checked = 0;
}

/**
 * @brief Acts as a game. The function calls the probed function at a fixed rate.
 *
 * Use it with the command "attach -l <this executable> -s fas_probe_frame" from
 * another shell to see the events of a listener without a real game.
 *
 * @param fps The frame rate.
 * @param seconds The duration.
 * @return 0 on success. Otherwise 1.
 */
int fas_selftest_frames(unsigned fps, double seconds) {
    struct scenario *s = calloc(1, sizeof(*s));

    if (!s || fps == 0 || fps > 1000) {
        free(s);
        return 1;
    }

    s->next = now_ns();
    frames(s, fps, seconds);
    free(s);
    return 0;
}

/**
 * @brief Runs the end-to-end test on this device.
 *
 * The test registers a listener on its own process. The probed function is
 * fas_probe_frame in this executable. The test then runs scenarios with known
 * frame timing and checks the events.
 *
 * @param quick Non-zero shortens the test.
 * @return 0 when all checks pass. Otherwise 1.
 */
int fas_selftest_run(int quick) {
    static const char *const symbols[] = {"fas_probe_frame"};
    struct scenario *s = calloc(1, sizeof(*s));
    struct fas_config cfg;
    const uint32_t f[] = {60};
    char path[FAS_MAX_PATH_LEN];
    uint64_t off;
    ssize_t len;
    int failed = 0;

    if (!s)
        return 1;

    len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0) {
        perror("readlink /proc/self/exe");
        return 1;
    }
    path[len] = '\0';

    if (fas_elf_resolve_offset(path, symbols, 1, &off) != 0) {
        fprintf(stderr, "cannot find fas_probe_frame in %s\n", path);
        return 1;
    }

    s->fd = fas_client_open(O_NONBLOCK);
    if (s->fd < 0) {
        perror("open " FAS_DEV_PATH);
        return 1;
    }

    fas_client_config_init(&cfg, f, 1, 60, 0);
    if (fas_client_register(s->fd, getpid(), path, off, &cfg, &s->ctx) != 0) {
        perror("register");
        return 1;
    }
    printf("listener %d on %s @ 0x%" PRIx64 "\n", s->ctx, path, off);

    struct {
        const char *name;
        void (*run)(struct scenario *);
        int slow;
    } list[] = {
        {"healthy 60 fps", sc_healthy, 0}, {"single hitches", sc_hitch, 0}, {"frame rate deficit", sc_deficit, 0},
        {"pause and resume", sc_pause, 0}, {"rate switching and lock", sc_rates, 1},
    };

    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        if (quick && list[i].slow) {
            printf("[SKIP] %s\n", list[i].name);
            continue;
        }
        s->failed = 0;
        list[i].run(s);
        printf("[%s] %s\n", s->failed ? "FAIL" : "PASS", list[i].name);
        failed += s->failed != 0;
    }

    s->failed = 0;
    sc_isolation(s, path, off);
    printf("[%s] process isolation\n", s->failed ? "FAIL" : "PASS");
    failed += s->failed != 0;

    if (!quick) {
        s->failed = 0;
        sc_stress(s, path, off);
        printf("[%s] concurrent frames and changes\n", s->failed ? "FAIL" : "PASS");
        failed += s->failed != 0;
    }

    s->failed = 0;
    sc_api(s, path, off, quick);
    printf("[%s] interface and cleanup\n", s->failed ? "FAIL" : "PASS");
    failed += s->failed != 0;

    fas_client_remove(s->fd, s->ctx);
    printf("%s\n", failed ? "SELFTEST FAILED" : "SELFTEST PASSED");
    free(s);
    return failed != 0;
}
