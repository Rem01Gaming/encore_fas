#ifndef _UAPI_ENCORE_FAS_H
#define _UAPI_ENCORE_FAS_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define FAS_MAX_PATH_LEN 256

struct fas_libgui_offset {
    __u64 offset;
    char path[FAS_MAX_PATH_LEN];
};

struct fas_register_args {
    __s32 pid;
    __s32 ctx_id;
};

struct fas_frametime_hint {
    __s32 ctx_id;
    __u32 fps;
};

enum fas_event_type {
    FAS_EVENT_FRAME_OK = 0,
    FAS_EVENT_SMALL_JANK = 1,
    FAS_EVENT_BIG_JANK = 2,
    FAS_EVENT_BOOST_SOFT = 3,
    FAS_EVENT_BOOST_HARD = 4,
};

struct fas_jank_event {
    __s32 ctx_id;
    __u32 type;
    __u64 timestamp_ms;
    __u64 frametime_ms;
};

#define FAS_IOC_MAGIC 'F'

#define FAS_IOC_UPDATE_LIBGUI_OFFSET  _IOW(FAS_IOC_MAGIC, 1, struct fas_libgui_offset)
#define FAS_IOC_GET_LIBGUI_OFFSET     _IOR(FAS_IOC_MAGIC, 2, struct fas_libgui_offset)
#define FAS_IOC_REGISTER_LISTENER     _IOWR(FAS_IOC_MAGIC, 3, struct fas_register_args)
#define FAS_IOC_REMOVE_LISTENER       _IOW(FAS_IOC_MAGIC, 4, __s32)
#define FAS_IOC_HINT_FRAMETIME        _IOW(FAS_IOC_MAGIC, 5, struct fas_frametime_hint)

#endif
