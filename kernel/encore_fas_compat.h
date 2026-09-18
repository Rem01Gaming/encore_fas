#ifndef _ENCORE_FAS_COMPAT_H
#define _ENCORE_FAS_COMPAT_H

#include <linux/version.h>
#include <linux/poll.h>
#include <linux/hrtimer.h>
#include <linux/uprobes.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
typedef unsigned int fas_poll_t;
#else
typedef __poll_t fas_poll_t;
#endif

static inline void fas_uprobe_unregister(struct inode *inode, loff_t offset,
					 struct uprobe_consumer *uc)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	uprobe_unregister_nosync(inode, offset, uc);
	uprobe_unregister_sync();
#else
	uprobe_unregister(inode, offset, uc);
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define fas_hrtimer_setup(timer, fn, clock, mode) \
	hrtimer_setup((timer), (fn), (clock), (mode))
#else
#define fas_hrtimer_setup(timer, fn, clock, mode)       \
	do {                                            \
		hrtimer_init((timer), (clock), (mode)); \
		(timer)->function = (fn);               \
	} while (0)
#endif

#endif
