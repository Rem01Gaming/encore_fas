#ifndef _ENCORE_FAS_EVENTQ_H
#define _ENCORE_FAS_EVENTQ_H

#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/types.h>

#include "encore_fas_compat.h"
#include "uapi/encore_fas_uapi.h"

int fas_eventq_init(void);
void fas_eventq_exit(void);

void fas_eventq_push(int ctx_id, enum fas_event_type type, u64 frametime_ms);

ssize_t fas_eventq_read(struct file *file, char __user *buf, size_t count,
			loff_t *ppos);
fas_poll_t fas_eventq_poll(struct file *file, poll_table *wait);

#endif
