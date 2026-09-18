// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include "encore_fas_evq.h"

#define FAS_EVQ_SIZE 512
#define FAS_EVQ_MASK (FAS_EVQ_SIZE - 1)
#define FAS_READ_BATCH 8

static struct {
	raw_spinlock_t lock;
	struct mutex read_lock;
	wait_queue_head_t wq;
	u32 head;
	u32 tail;
	u32 dropped;
	struct fas_event buf[FAS_EVQ_SIZE];
} evq;

/**
 * @brief Prepares the queue.
 */
void fas_evq_init(void)
{
	BUILD_BUG_ON(sizeof(struct fas_event) != 48);
	BUILD_BUG_ON(FAS_EVQ_SIZE & FAS_EVQ_MASK);

	raw_spin_lock_init(&evq.lock);
	mutex_init(&evq.read_lock);
	init_waitqueue_head(&evq.wq);
	evq.head = 0;
	evq.tail = 0;
	evq.dropped = 0;
}

/**
 * @brief Copies up to @max pending events without removing them.
 *
 * @param out The array that receives the events.
 * @param max The size of @out.
 * @return The number of events copied.
 */
static u32 fas_evq_peek(struct fas_event *out, u32 max)
{
	unsigned long flags;
	u32 n, i;

	raw_spin_lock_irqsave(&evq.lock, flags);
	n = min_t(u32, evq.head - evq.tail, max);
	for (i = 0; i < n; i++)
		out[i] = evq.buf[(evq.tail + i) & FAS_EVQ_MASK];
	raw_spin_unlock_irqrestore(&evq.lock, flags);

	return n;
}

/**
 * @brief Removes up to @n events that a prior fas_evq_peek() returned.
 *
 * @param n The number of events to remove.
 */
static void fas_evq_release(u32 n)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&evq.lock, flags);
	/* Clamp in case an overflow already advanced tail past our peek. */
	evq.tail += min_t(u32, n, evq.head - evq.tail);
	raw_spin_unlock_irqrestore(&evq.lock, flags);
}

/**
 * @brief Adds an event to the queue.
 *
 * @param ev The event to copy.
 */
void fas_evq_push(const struct fas_event *ev)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&evq.lock, flags);
	if (unlikely(evq.head - evq.tail == FAS_EVQ_SIZE)) {
		evq.tail++;
		evq.dropped++;
	}
	evq.buf[evq.head & FAS_EVQ_MASK] = *ev;
	evq.head++;
	raw_spin_unlock_irqrestore(&evq.lock, flags);
}

/**
 * @brief Wakes the readers. Call it after fas_evq_push().
 */
void fas_evq_wake(void)
{
	wake_up_interruptible(&evq.wq);
}

/**
 * @brief Reads the count of dropped events.
 *
 * @return The number of events that the queue dropped.
 */
u32 fas_evq_dropped(void)
{
	return READ_ONCE(evq.dropped);
}

static bool fas_evq_nonempty(void)
{
	return READ_ONCE(evq.head) != READ_ONCE(evq.tail);
}

/**
 * @brief Implements read().
 *
 * @param file The file.
 * @param buf The user buffer. It must hold at least one event.
 * @param count The size of @buf in bytes.
 * @param ppos Not used.
 * @return The number of bytes copied, or a negative error code.
 */
ssize_t fas_evq_read(struct file *file, char __user *buf, size_t count,
		     loff_t *ppos)
{
	struct fas_event batch[FAS_READ_BATCH];
	u32 n;
	ssize_t ret;

	if (count < sizeof(struct fas_event))
		return -EINVAL;

	if (mutex_lock_interruptible(&evq.read_lock))
		return -ERESTARTSYS;

	for (;;) {
		n = fas_evq_peek(batch,
				 min_t(size_t, count / sizeof(*batch), FAS_READ_BATCH));
		if (n)
			break;

		if (file->f_flags & O_NONBLOCK) {
			ret = -EAGAIN;
			goto out;
		}

		if (wait_event_interruptible(evq.wq, fas_evq_nonempty())) {
			ret = -ERESTARTSYS;
			goto out;
		}
	}

	if (copy_to_user(buf, batch, n * sizeof(*batch))) {
		ret = -EFAULT;   /* events stay in the queue, nothing lost */
		goto out;
	}

	fas_evq_release(n);
	ret = n * sizeof(*batch);

out:
	mutex_unlock(&evq.read_lock);
	return ret;
}

/**
 * @brief Implements poll().
 *
 * @param file The file.
 * @param wait The poll table.
 * @return The readable mask when an event exists.
 */
fas_poll_t fas_evq_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &evq.wq, wait);
	return fas_evq_nonempty() ? FAS_POLLIN : 0;
}
