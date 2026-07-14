#include "fas_client.h"

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int fas_client_open(void) {
    return open(FAS_DEV_PATH, O_RDWR);
}

int fas_client_set_libgui_offset(int fd, const char *path, uint64_t offset) {
    struct fas_libgui_offset req = { .offset = offset };

    strncpy(req.path, path, FAS_MAX_PATH_LEN - 1);
    req.path[FAS_MAX_PATH_LEN - 1] = '\0';

    return ioctl(fd, FAS_IOC_UPDATE_LIBGUI_OFFSET, &req);
}

int fas_client_get_libgui_offset(int fd, struct fas_libgui_offset *out) {
    return ioctl(fd, FAS_IOC_GET_LIBGUI_OFFSET, out);
}

int fas_client_register_listener(int fd, int pid, int *out_ctx_id) {
    struct fas_register_args args = { .pid = pid };
    int ret = ioctl(fd, FAS_IOC_REGISTER_LISTENER, &args);

    if (ret == 0 && out_ctx_id)
        *out_ctx_id = args.ctx_id;

    return ret;
}

int fas_client_remove_listener(int fd, int ctx_id) {
    return ioctl(fd, FAS_IOC_REMOVE_LISTENER, &ctx_id);
}

int fas_client_hint_frametime(int fd, int ctx_id, uint32_t fps) {
    struct fas_frametime_hint hint = { .ctx_id = ctx_id, .fps = fps };

    return ioctl(fd, FAS_IOC_HINT_FRAMETIME, &hint);
}

int fas_client_read_event(int fd, struct fas_jank_event *out) {
    ssize_t n = read(fd, out, sizeof(*out));

    return (n == (ssize_t)sizeof(*out)) ? 0 : -1;
}
