#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/uaccess.h>

#include "encore_fas_eventq.h"
#include "encore_fas_time.h"

#define FAS_EVENT_FIFO_SIZE 1024

static DEFINE_SPINLOCK(fas_fifo_lock);
static DECLARE_KFIFO(fas_event_fifo, struct fas_jank_event,
		     FAS_EVENT_FIFO_SIZE);
static DECLARE_WAIT_QUEUE_HEAD(fas_poll_wait);

int fas_eventq_init(void)
{
	INIT_KFIFO(fas_event_fifo);
	return 0;
}

void fas_eventq_exit(void)
{
}

void fas_eventq_push(int ctx_id, enum fas_event_type type, u64 frametime_ms)
{
	struct fas_jank_event ev = {
		.ctx_id = ctx_id,
		.type = type,
		.timestamp_ms = fas_now_ms(),
		.frametime_ms = frametime_ms,
	};
	unsigned long flags;

	spin_lock_irqsave(&fas_fifo_lock, flags);
	kfifo_put(&fas_event_fifo, ev);
	spin_unlock_irqrestore(&fas_fifo_lock, flags);

	wake_up_interruptible(&fas_poll_wait);
}

ssize_t fas_eventq_read(struct file *file, char __user *buf, size_t count,
			loff_t *ppos)
{
	struct fas_jank_event ev;
	unsigned long flags;
	int got;

	if (count < sizeof(ev))
		return -EINVAL;

	while (true) {
		spin_lock_irqsave(&fas_fifo_lock, flags);
		got = kfifo_get(&fas_event_fifo, &ev);
		spin_unlock_irqrestore(&fas_fifo_lock, flags);

		if (got)
			break;

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(fas_poll_wait,
					     !kfifo_is_empty(&fas_event_fifo)))
			return -ERESTARTSYS;
	}

	if (copy_to_user(buf, &ev, sizeof(ev)))
		return -EFAULT;

	return sizeof(ev);
}

fas_poll_t fas_eventq_poll(struct file *file, poll_table *wait)
{
	poll_wait(file, &fas_poll_wait, wait);

	if (!kfifo_is_empty(&fas_event_fifo))
		return POLLIN | POLLRDNORM;

	return 0;
}
