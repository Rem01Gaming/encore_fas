// SPDX-License-Identifier: GPL-2.0-only

#ifndef ENCORE_FAS_COMPAT_H
#define ENCORE_FAS_COMPAT_H

#include <linux/err.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/poll.h>
#include <linux/uprobes.h>
#include <linux/version.h>

#ifndef CONFIG_ARM64
#error "encore_fas supports arm64 only."
#endif

/*
 * Kernel up to v6.11. Register and unregister take an inode and an offset.
 * Kernel v6.12. Register returns a struct uprobe pointer.
 * Kernel v6.13 and later. The handler also takes a cookie pointer.
 */
#ifndef FAS_UPROBE_API
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
#define FAS_UPROBE_API 3
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define FAS_UPROBE_API 2
#else
#define FAS_UPROBE_API 1
#endif
#endif

#if FAS_UPROBE_API >= 3
#define FAS_UPROBE_HANDLER_ARGS \
	struct uprobe_consumer *uc, struct pt_regs *regs, __u64 *cookie
#else
#define FAS_UPROBE_HANDLER_ARGS struct uprobe_consumer *uc, struct pt_regs *regs
#endif

#if FAS_UPROBE_API >= 2
#define FAS_UPROBE_FILTER_ARGS struct uprobe_consumer *uc, struct mm_struct *mm
#else
#define FAS_UPROBE_FILTER_ARGS                                    \
	struct uprobe_consumer *uc, enum uprobe_filter_ctx fctx, \
		struct mm_struct *mm
#endif

/**
 * @brief Handle of one registered uprobe.
 */
struct fas_probe {
	/** Inode of the probed file. The old API needs it to unregister. */
	struct inode *inode;
	/** File offset of the probe. The old API needs it to unregister. */
	loff_t offset;
	/** Handle that the new API returns. */
	struct uprobe *uprobe;
};

/**
 * @brief Attaches a consumer to a file offset.
 *
 * @param probe The handle to fill.
 * @param inode Inode of the probed file.
 * @param offset File offset of the probed instruction.
 * @param uc The consumer.
 * @return 0 on success. Otherwise a negative error code.
 */
static inline int fas_uprobe_register(struct fas_probe *probe,
				      struct inode *inode, loff_t offset,
				      struct uprobe_consumer *uc)
{
	probe->inode = inode;
	probe->offset = offset;
	probe->uprobe = NULL;

#if FAS_UPROBE_API >= 2
	probe->uprobe = uprobe_register(inode, offset, 0, uc);
	if (IS_ERR(probe->uprobe)) {
		int ret = PTR_ERR(probe->uprobe);

		probe->uprobe = NULL;
		return ret;
	}
	return 0;
#else
	return uprobe_register(inode, offset, uc);
#endif
}

/**
 * @brief Detaches a consumer and waits for its handlers.
 *
 * @param probe The handle from fas_uprobe_register().
 * @param uc The consumer.
 * @note When this function returns, no handler of @uc is running. 
 */
static inline void fas_uprobe_unregister(struct fas_probe *probe,
					 struct uprobe_consumer *uc)
{
#if FAS_UPROBE_API >= 2
	uprobe_unregister_nosync(probe->uprobe, uc);
	uprobe_unregister_sync();
#else
	uprobe_unregister(probe->inode, probe->offset, uc);
#endif
}

#if defined(FAS_HAVE_HRTIMER_SETUP) || \
	LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
#define fas_hrtimer_setup(timer, fn, clock, mode) \
	hrtimer_setup((timer), (fn), (clock), (mode))
#else
#define fas_hrtimer_setup(timer, fn, clock, mode)       \
	do {                                            \
		hrtimer_init((timer), (clock), (mode)); \
		(timer)->function = (fn);               \
	} while (0)
#endif

/*
 * The watchdog callback runs in softirq context when the kernel has softirq
 * timers (v4.16 and later). Then it can wake a reader without a hardirq wait
 * on a spinlock_t, and the code is correct on a real-time kernel too.
 */
#if defined(FAS_HAVE_HRTIMER_SOFT) || \
	LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
#define FAS_TIMER_MODE HRTIMER_MODE_REL_PINNED_SOFT
#else
#define FAS_TIMER_MODE HRTIMER_MODE_REL_PINNED
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
typedef unsigned int fas_poll_t;
#define FAS_POLLIN (POLLIN | POLLRDNORM)
#else
typedef __poll_t fas_poll_t;
#define FAS_POLLIN ((__force __poll_t)(EPOLLIN | EPOLLRDNORM))
#endif

#endif
