#ifndef _ENCORE_FAS_JANK_H
#define _ENCORE_FAS_JANK_H

#include <linux/types.h>
#include <linux/sched.h>

#include "uapi/encore_fas_uapi.h"

int fas_jank_init(void);
void fas_jank_exit(void);

int fas_jank_set_libgui_offset(u64 offset, const char *path);
void fas_jank_get_libgui_offset(struct fas_libgui_offset *out);

long fas_jank_register_listener(pid_t pid, int *out_ctx_id);
long fas_jank_remove_listener(int ctx_id);
long fas_jank_hint_frametime(int ctx_id, u32 fps);
long fas_jank_set_target_list(int ctx_id,
			      const struct fas_target_fps_list *req);

#endif
