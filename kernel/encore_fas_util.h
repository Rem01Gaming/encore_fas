#ifndef _ENCORE_FAS_UTIL_H
#define _ENCORE_FAS_UTIL_H

#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/sched.h>

static inline bool fas_caller_is_root(void)
{
	return uid_eq(current_euid(), GLOBAL_ROOT_UID);
}

#endif
