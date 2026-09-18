#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fas_client.h"
#include "fas_elf.h"
#include "fas_selftest.h"

#define DEFAULT_LIBGUI "/system/lib64/libgui.so"

static const char *g_symbol;

static const char *const kQueueBufferSymbols[] = {
    "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferi",
    "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferiPNS_24SurfaceQueueBufferOutputE",
};

/**
 * @brief Prints the command list to stderr.
 *
 * @param argv0 The program name.
 */
static void print_usage(const char *argv0) {
    fprintf(stderr,
            "usage:\n"
            "  %1$s version\n"
            "  %1$s attach <pid> [-l lib] [-o hex_offset | -s symbol] [-v vsync_hz] [-L] <fps>...\n"
            "  %1$s config <ctx_id> [-v vsync_hz] [-L] <fps>...\n"
            "  %1$s state <ctx_id>\n"
            "  %1$s detach <ctx_id>\n"
            "  %1$s list\n"
            "  %1$s listen [-c count]\n"
            "  %1$s selftest [-q]\n"
            "  %1$s frames <fps> <seconds>\n"
            "\n"
            "  -l  file with Surface::queueBuffer (default " DEFAULT_LIBGUI ")\n"
            "  -o  file offset of queueBuffer. Without it, the tool reads the symbol table.\n"
            "  -s  symbol to probe instead of Surface::queueBuffer\n"
            "  -v  refresh rate of the display in hertz\n"
            "  -L  never switch to a slower target\n",
            argv0);
}

/**
 * @brief Reads the target options and the frame rate list.
 *
 * @param argc The number of arguments after the command name.
 * @param argv The arguments after the command name.
 * @param cfg The structure that receives the configuration.
 * @param lib The variable that receives the -l value. This parameter can be NULL.
 * @param offset The variable that receives the -o value. This parameter can be NULL.
 * @param have_offset The variable that is set when the user gave -o. This parameter can be NULL.
 * @return 0 on success. Otherwise -1.
 */
static int parse_targets(int argc, char **argv, struct fas_config *cfg, const char **lib, uint64_t *offset, int *have_offset) {
    uint32_t fps[FAS_MAX_TARGETS];
    uint32_t count = 0, vsync = 0;
    int lock = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-L")) {
            lock = 1;
        } else if (!strcmp(argv[i], "-v") && i + 1 < argc) {
            vsync = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-l") && lib && i + 1 < argc) {
            *lib = argv[++i];
        } else if (!strcmp(argv[i], "-s") && lib && i + 1 < argc) {
            g_symbol = argv[++i];
        } else if (!strcmp(argv[i], "-o") && offset && i + 1 < argc) {
            *offset = strtoull(argv[++i], NULL, 16);
            *have_offset = 1;
        } else if (argv[i][0] != '-' && count < FAS_MAX_TARGETS) {
            fps[count++] = (uint32_t)atoi(argv[i]);
        } else {
            fprintf(stderr, "bad argument: %s\n", argv[i]);
            return -1;
        }
    }

    if (count == 0) {
        fprintf(stderr, "give at least one frame rate\n");
        return -1;
    }

    fas_client_config_init(cfg, fps, count, vsync, lock);
    return 0;
}

static int cmd_version(int fd) {
    struct fas_version ver;

    if (fas_client_get_version(fd, &ver) != 0) {
        perror("version");
        return 1;
    }

    printf("version: %u\nabi: %u\ncounter: %u Hz\n", ver.version, ver.abi, ver.counter_hz);
    if (ver.abi != FAS_ABI_VERSION)
        fprintf(stderr, "warning: this tool uses abi %d\n", FAS_ABI_VERSION);
    return 0;
}

/**
 * @brief Attaches a listener to a process and prints the listener ID.
 *
 * @param fd The file descriptor of the FAS device.
 * @param argc The number of arguments after the command name.
 * @param argv The arguments after the command name.
 * @return 0 on success. Otherwise 1.
 */
static int cmd_attach(int fd, int argc, char **argv) {
    struct fas_config cfg;
    const char *lib = DEFAULT_LIBGUI;
    uint64_t offset = 0;
    int have_offset = 0, ctx_id;

    if (argc < 2 || parse_targets(argc - 1, argv + 1, &cfg, &lib, &offset, &have_offset) != 0) {
        fprintf(stderr, "attach needs a pid and at least one frame rate\n");
        return 1;
    }

    const char *const *symbols = kQueueBufferSymbols;
    size_t symbol_count = sizeof(kQueueBufferSymbols) / sizeof(kQueueBufferSymbols[0]);

    if (g_symbol) {
        symbols = &g_symbol;
        symbol_count = 1;
    }

    if (!have_offset && fas_elf_resolve_offset(lib, symbols, symbol_count, &offset) != 0) {
        fprintf(stderr,
                "cannot find the symbol in %s\n"
                "run `readelf -sW %s | grep queueBuffer` and pass the offset with -o\n",
                lib, lib);
        return 1;
    }

    if (fas_client_register(fd, atoi(argv[0]), lib, offset, &cfg, &ctx_id) != 0) {
        perror("attach");
        return 1;
    }

    printf("ctx_id: %d\n", ctx_id);
    return 0;
}

static int cmd_config(int fd, int argc, char **argv) {
    struct fas_config cfg;

    if (argc < 2 || parse_targets(argc - 1, argv + 1, &cfg, NULL, NULL, NULL) != 0) {
        fprintf(stderr, "config needs a ctx_id and at least one frame rate\n");
        return 1;
    }

    if (fas_client_set_config(fd, atoi(argv[0]), &cfg) != 0) {
        perror("config");
        return 1;
    }
    return 0;
}

static int cmd_state(int fd, int argc, char **argv) {
    struct fas_state st;

    if (argc < 1) {
        fprintf(stderr, "state needs a ctx_id\n");
        return 1;
    }

    if (fas_client_get_state(fd, atoi(argv[0]), &st) != 0) {
        perror("state");
        return 1;
    }

    printf("fps: %u\nacquiring: %d\ndegraded: %d\npaused: %d\npressure: %.3f\nseq: %u\ndropped: %u\n", st.fps, !!(st.flags & FAS_STATE_ACQUIRING),
           !!(st.flags & FAS_STATE_DEGRADED), !!(st.flags & FAS_STATE_PAUSED), st.pressure_q16 / 65536.0, st.seq, st.dropped);
    return 0;
}

static int cmd_detach(int fd, int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr, "detach needs a ctx_id\n");
        return 1;
    }

    if (fas_client_remove(fd, atoi(argv[0])) != 0) {
        perror("detach");
        return 1;
    }
    return 0;
}

static int cmd_list(int fd) {
    struct fas_listener_list list;

    if (fas_client_list(fd, &list) != 0) {
        perror("list");
        return 1;
    }

    for (uint32_t i = 0; i < list.count; i++)
        printf("ctx_id: %d pid: %d\n", list.listeners[i].ctx_id, list.listeners[i].pid);
    return 0;
}

/**
 * @brief Prints events until the read fails or the count ends.
 *
 * @param fd The file descriptor of the FAS device.
 * @param argc The number of arguments after the command name.
 * @param argv The arguments after the command name.
 * @return 0 after the count ends. Otherwise 1.
 */
static int cmd_listen(int fd, int argc, char **argv) {
    struct fas_event ev[8];
    long limit = -1;

    if (argc >= 2 && !strcmp(argv[0], "-c"))
        limit = atol(argv[1]);

    while (limit != 0) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};

        if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
            perror("poll");
            return 1;
        }

        ssize_t n = fas_client_read_events(fd, ev, 8);

        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            perror("read");
            return 1;
        }

        for (ssize_t i = 0; i < n && limit != 0; i++, limit--) {
            uint64_t ts_us = ev[i].timestamp_ns / 1000, ft_us = ev[i].frametime_ns / 1000;

            printf("ctx=%d seq=%u %-11s ts=%" PRIu64 ".%03" PRIu64 "ms frametime=%" PRIu64 ".%03" PRIu64 "ms fps=%u missed=%u pressure=%.2f%s\n", ev[i].ctx_id,
                   ev[i].seq, fas_client_event_name(ev[i].type), ts_us / 1000, ts_us % 1000, ft_us / 1000, ft_us % 1000, ev[i].fps, ev[i].missed,
                   ev[i].pressure_q16 / 65536.0, (ev[i].flags & FAS_EVF_WATCHDOG) ? " wd" : "");
        }
        fflush(stdout);
    }
    return 0;
}

/**
 * @brief Runs the command from the command line.
 *
 * @param argc The number of command-line arguments.
 * @param argv The command-line arguments.
 * @return 0 on success. Otherwise 1.
 */
int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (!strcmp(cmd, "selftest"))
        return fas_selftest_run(argc > 2 && !strcmp(argv[2], "-q"));

    if (!strcmp(cmd, "frames") && argc >= 4)
        return fas_selftest_frames((unsigned)atoi(argv[2]), atof(argv[3]));

    int fd = fas_client_open(0);

    if (fd < 0) {
        perror("open " FAS_DEV_PATH);
        fprintf(stderr, "is the module loaded and are you root?\n");
        return 1;
    }

    int ret;

    if (!strcmp(cmd, "version"))
        ret = cmd_version(fd);
    else if (!strcmp(cmd, "attach"))
        ret = cmd_attach(fd, argc - 2, argv + 2);
    else if (!strcmp(cmd, "config"))
        ret = cmd_config(fd, argc - 2, argv + 2);
    else if (!strcmp(cmd, "state"))
        ret = cmd_state(fd, argc - 2, argv + 2);
    else if (!strcmp(cmd, "detach"))
        ret = cmd_detach(fd, argc - 2, argv + 2);
    else if (!strcmp(cmd, "list"))
        ret = cmd_list(fd);
    else if (!strcmp(cmd, "listen"))
        ret = cmd_listen(fd, argc - 2, argv + 2);
    else {
        print_usage(argv[0]);
        ret = 1;
    }

    close(fd);
    return ret;
}
