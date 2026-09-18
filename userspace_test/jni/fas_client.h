#ifndef FAS_CLIENT_H
#define FAS_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <uapi/encore_fas_uapi.h>

#define FAS_DEV_PATH "/dev/encore_fas"

int fas_client_open(int flags);

int fas_client_get_version(int fd, struct fas_version *out);

void fas_client_config_init(struct fas_config *cfg, const uint32_t *fps, uint32_t count, uint32_t vsync_hz, int lock_down);

int fas_client_register(int fd, int pid, const char *path, uint64_t offset, const struct fas_config *cfg, int *out_ctx_id);

int fas_client_remove(int fd, int ctx_id);

int fas_client_set_config(int fd, int ctx_id, const struct fas_config *cfg);

int fas_client_get_state(int fd, int ctx_id, struct fas_state *out);

int fas_client_list(int fd, struct fas_listener_list *out);

ssize_t fas_client_read_events(int fd, struct fas_event *out, size_t max);

const char *fas_client_event_name(uint32_t type);

#endif
