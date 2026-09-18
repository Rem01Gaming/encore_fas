#ifndef FAS_CLIENT_H
#define FAS_CLIENT_H

#include <stdint.h>

#include <uapi/encore_fas_uapi.h>

#define FAS_DEV_PATH "/dev/encore_fas"

int fas_client_open(void);

int fas_client_get_version(int fd, struct fas_version *out);

int fas_client_set_libgui_offset(int fd, const char *path, uint64_t offset);

int fas_client_get_libgui_offset(int fd, struct fas_libgui_offset *out);

int fas_client_register_listener(int fd, int pid, int *out_ctx_id);

int fas_client_remove_listener(int fd, int ctx_id);

int fas_client_hint_frametime(int fd, int ctx_id, uint32_t fps);

int fas_client_set_target_fps_list(int fd, int ctx_id, const uint32_t *fps, uint32_t count);

int fas_client_read_event(int fd, struct fas_jank_event *out);

#endif
