#include "fas_client.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/**
 * @brief Opens the FAS device.
 *
 * @return A file descriptor on success. Otherwise -1, and errno has the error code.
 */
int fas_client_open(void) {
    return open(FAS_DEV_PATH, O_RDWR);
}

/**
 * Reads the version of the kernel module.
 *
 * @param fd The file descriptor of the FAS device.
 * @param out The structure that receives the version.
 * @return 0 on success. Otherwise -1, and errno has the error code.
 */
int fas_client_get_version(int fd, struct fas_version *out) {
    return ioctl(fd, FAS_IOC_GET_VERSION, out);
}

/**
 * @brief Sets the libgui path and the queueBuffer offset that the module probes.
 *
 * @param fd The file descriptor of the FAS device.
 * @param path The path of the libgui library. The function truncates the path to FAS_MAX_PATH_LEN - 1 bytes.
 * @param offset The file offset of the queueBuffer function.
 * @return 0 on success. Otherwise -1, and errno has the error code.
 */
int fas_client_set_libgui_offset(int fd, const char *path, uint64_t offset) {
    struct fas_libgui_offset req = {.offset = offset};

    strncpy(req.path, path, FAS_MAX_PATH_LEN - 1);
    req.path[FAS_MAX_PATH_LEN - 1] = '\0';

    return ioctl(fd, FAS_IOC_UPDATE_LIBGUI_OFFSET, &req);
}

/**
 * @brief Reads the libgui path and the queueBuffer offset from the module.
 *
 * @param fd The file descriptor of the FAS device.
 * @param out The structure that receives the path and the offset.
 * @return 0 on success. Otherwise -1, and errno has the error code.
 */
int fas_client_get_libgui_offset(int fd, struct fas_libgui_offset *out) {
    return ioctl(fd, FAS_IOC_GET_LIBGUI_OFFSET, out);
}

/**
 * @brief Attaches a listener to a process.
 *
 * @param fd The file descriptor of the FAS device.
 * @param pid The process ID of the game.
 * @param out_ctx_id The variable that receives the context ID. This parameter can be NULL.
 * @return 0 on success. Otherwise -1, and errno has the error code.
 */
int fas_client_register_listener(int fd, int pid, int *out_ctx_id) {
    struct fas_register_args args = {.pid = pid};
    int ret = ioctl(fd, FAS_IOC_REGISTER_LISTENER, &args);

    if (ret == 0 && out_ctx_id)
        *out_ctx_id = args.ctx_id;

    return ret;
}

/**
 * @brief Detaches a listener from its process.
 *
 * @param fd The file descriptor of the FAS device.
 * @param ctx_id The context ID from fas_client_register_listener.
 * @return 0 on success. Otherwise -1, and errno has the error code.
 */
int fas_client_remove_listener(int fd, int ctx_id) {
    struct fas_remove_args args = {.ctx_id = ctx_id};

    return ioctl(fd, FAS_IOC_REMOVE_LISTENER, &args);
}

/**
 * @brief Sets the baseline frame rate of a listener.
 *
 * @param fd The file descriptor of the FAS device.
 * @param ctx_id The context ID from fas_client_register_listener.
 * @param fps The frame rate hint in frames per second.
 * @return 0 on success. Otherwise -1, and errno has the error code.
 */
int fas_client_hint_frametime(int fd, int ctx_id, uint32_t fps) {
    struct fas_frametime_hint hint = {.ctx_id = ctx_id, .fps = fps};

    return ioctl(fd, FAS_IOC_HINT_FRAMETIME, &hint);
}

/**
 * @brief Sets the list of target frame rates of a listener.
 *
 * A count of 0 clears the list. The module then detects the frame rate by itself.
 *
 * @param fd The file descriptor of the FAS device.
 * @param ctx_id The context ID from fas_client_register_listener.
 * @param fps The target frame rates in frames per second. This parameter can be NULL when count is 0.
 * @param count The number of values in fps. The maximum is FAS_MAX_TARGETS.
 * @return 0 on success. Otherwise -1, and errno has the error code. The module rejects a value of 0 or more than 1000 with EINVAL.
 */
int fas_client_set_target_fps_list(int fd, int ctx_id, const uint32_t *fps, uint32_t count) {
    struct fas_target_fps_list req = {.ctx_id = ctx_id, .count = count};

    if (count > FAS_MAX_TARGETS || (count > 0 && fps == NULL)) {
        errno = EINVAL;
        return -1;
    }

    for (uint32_t i = 0; i < count; i++)
        req.fps[i] = fps[i];

    return ioctl(fd, FAS_IOC_SET_TARGET_FPS_LIST, &req);
}

/**
 * @brief Reads one jank event from the module. The call blocks until an event is available.
 *
 * @param fd The file descriptor of the FAS device.
 * @param out The structure that receives the event.
 * @return 0 on success. Otherwise -1.
 */
int fas_client_read_event(int fd, struct fas_jank_event *out) {
    ssize_t n = read(fd, out, sizeof(*out));

    return (n == (ssize_t)sizeof(*out)) ? 0 : -1;
}
