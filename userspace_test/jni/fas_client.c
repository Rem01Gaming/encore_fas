#include "fas_client.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/**
 * @brief Opens the FAS device.
 *
 * @param flags Extra flags for open(). Use O_NONBLOCK for a non-blocking read.
 * @return A file descriptor on success. Otherwise -1, and errno has the error code.
 */
int fas_client_open(int flags) {
    return open(FAS_DEV_PATH, O_RDWR | O_CLOEXEC | flags);
}

/**
 * @brief Reads the version of the kernel module.
 *
 * @param fd The file descriptor of the FAS device.
 * @param out The structure that receives the version.
 * @return 0 on success. Otherwise -1, and errno has the error code.
 */
int fas_client_get_version(int fd, struct fas_version *out) {
    return ioctl(fd, FAS_IOC_GET_VERSION, out);
}

/**
 * @brief Fills a configuration structure.
 *
 * @param cfg The structure to fill.
 * @param fps The legal frame rates.
 * @param count The number of values in fps. The function keeps at most FAS_MAX_TARGETS values.
 * @param vsync_hz The refresh rate of the display. Zero selects the fastest target.
 * @param lock_down Non-zero forbids a switch to a slower target.
 */
void fas_client_config_init(struct fas_config *cfg, const uint32_t *fps, uint32_t count, uint32_t vsync_hz, int lock_down) {
    memset(cfg, 0, sizeof(*cfg));

    if (count > FAS_MAX_TARGETS)
        count = FAS_MAX_TARGETS;

    cfg->count = count;
    for (uint32_t i = 0; i < count; i++)
        cfg->fps[i] = fps[i];

    if (vsync_hz)
        cfg->vsync_ns = (uint32_t)(1000000000u / vsync_hz);
    if (lock_down)
        cfg->flags |= FAS_CFG_LOCK_DOWN;
}

/**
 * @brief Attaches a listener to a game process.
 *
 * @param fd The file descriptor of the FAS device.
 * @param pid The process ID of the game.
 * @param path The file that holds the probed function, for example libgui.so.
 * @param offset The file offset of the probed function.
 * @param cfg The initial targets.
 * @param out_ctx_id The variable that receives the listener ID. This parameter can be NULL.
 * @return 0 on success. Otherwise -1, and errno has the error code.
 */
int fas_client_register(int fd, int pid, const char *path, uint64_t offset, const struct fas_config *cfg, int *out_ctx_id) {
    struct fas_register_args args;

    if (strlen(path) >= FAS_MAX_PATH_LEN) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memset(&args, 0, sizeof(args));
    args.pid = pid;
    args.offset = offset;
    args.cfg = *cfg;
    strcpy(args.path, path);

    if (ioctl(fd, FAS_IOC_REGISTER, &args) != 0)
        return -1;

    if (out_ctx_id)
        *out_ctx_id = args.ctx_id;
    return 0;
}

/**
 * @brief Detaches a listener.
 *
 * @param fd The file descriptor of the FAS device.
 * @param ctx_id The listener ID.
 * @return 0 on success. Otherwise -1, and errno has the error code.
 */
int fas_client_remove(int fd, int ctx_id) {
    struct fas_remove_args args = {.ctx_id = ctx_id};

    return ioctl(fd, FAS_IOC_REMOVE, &args);
}

/**
 * @brief Replaces the targets of a listener. The listener then detects its active target again.
 *
 * @param fd The file descriptor of the FAS device.
 * @param ctx_id The listener ID.
 * @param cfg The new targets.
 * @return 0 on success. Otherwise -1, and errno has the error code. The module rejects an invalid list with EINVAL.
 */
int fas_client_set_config(int fd, int ctx_id, const struct fas_config *cfg) {
    struct fas_config_args args;

    memset(&args, 0, sizeof(args));
    args.ctx_id = ctx_id;
    args.cfg = *cfg;

    return ioctl(fd, FAS_IOC_SET_CONFIG, &args);
}

/**
 * @brief Reads the state of a listener.
 *
 * After a gap in the event sequence numbers, call this function to rebuild the state.
 *
 * @param fd The file descriptor of the FAS device.
 * @param ctx_id The listener ID.
 * @param out The structure that receives the state.
 * @return 0 on success. Otherwise -1, and errno has the error code.
 */
int fas_client_get_state(int fd, int ctx_id, struct fas_state *out) {
    memset(out, 0, sizeof(*out));
    out->ctx_id = ctx_id;

    return ioctl(fd, FAS_IOC_GET_STATE, out);
}

/**
 * @brief Lists the listeners that are attached now.
 *
 * Listeners stay attached until their process exits. After a daemon restart,
 * use this function to find them.
 *
 * @param fd The file descriptor of the FAS device.
 * @param out The structure that receives the list.
 * @return 0 on success. Otherwise -1, and errno has the error code.
 */
int fas_client_list(int fd, struct fas_listener_list *out) {
    return ioctl(fd, FAS_IOC_LIST, out);
}

/**
 * @brief Reads events from the module.
 *
 * A blocking device makes the call wait for at least one event.
 *
 * @param fd The file descriptor of the FAS device.
 * @param out The array that receives the events.
 * @param max The size of the array. The module returns at most 8 events per call.
 * @return The number of events on success. Otherwise -1, and errno has the error code.
 */
ssize_t fas_client_read_events(int fd, struct fas_event *out, size_t max) {
    ssize_t n = read(fd, out, max * sizeof(*out));

    if (n < 0)
        return -1;

    return n / (ssize_t)sizeof(*out);
}

/**
 * @brief Converts an event type to a text name.
 *
 * @param type The event type from struct fas_event.
 * @return A constant string. The string is "unknown" for an invalid type.
 */
const char *fas_client_event_name(uint32_t type) {
    static const char *const names[] = {
        [FAS_EVENT_NONE] = "none",
        [FAS_EVENT_SMALL_JANK] = "small_jank",
        [FAS_EVENT_BIG_JANK] = "big_jank",
        [FAS_EVENT_BOOST_SOFT] = "boost_soft",
        [FAS_EVENT_BOOST_HARD] = "boost_hard",
        [FAS_EVENT_DEGRADED] = "degraded",
        [FAS_EVENT_RECOVERED] = "recovered",
        [FAS_EVENT_PAUSED] = "paused",
        [FAS_EVENT_RESUMED] = "resumed",
        [FAS_EVENT_RATE_SWITCH] = "rate_switch",
    };

    if (type < sizeof(names) / sizeof(names[0]) && names[type])
        return names[type];
    return "unknown";
}
