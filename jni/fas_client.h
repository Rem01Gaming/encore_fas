#ifndef FAS_CLIENT_H
#define FAS_CLIENT_H

#include <stdint.h>

#include "encore_fas_uapi.h"

#define FAS_DEV_PATH "/dev/encore_fas"

/**
 * @brief Opens the driver device node for read/write ioctl access.
 * @return Open file descriptor, or -1 on failure (errno set).
 */
int fas_client_open(void);

/**
 * @brief Pushes a resolved libgui.so path and file offset to the driver.
 */
int fas_client_set_libgui_offset(int fd, const char *path, uint64_t offset);

/**
 * @brief Retrieves the driver's currently configured libgui offset.
 */
int fas_client_get_libgui_offset(int fd, struct fas_libgui_offset *out);

/**
 * @brief Attaches a jank listener to the given pid.
 * @param out_ctx_id Receives the listener handle on success.
 */
int fas_client_register_listener(int fd, int pid, int *out_ctx_id);

/**
 * @brief Detaches a previously registered listener.
 */
int fas_client_remove_listener(int fd, int ctx_id);

/**
 * @brief Hints the expected fps for a listener's frametime baseline.
 */
int fas_client_hint_frametime(int fd, int ctx_id, uint32_t fps);

/**
 * @brief Blocking read of a single jank event from the device.
 */
int fas_client_read_event(int fd, struct fas_jank_event *out);

#endif
