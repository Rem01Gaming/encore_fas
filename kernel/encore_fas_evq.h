// SPDX-License-Identifier: GPL-2.0-only

#ifndef ENCORE_FAS_EVQ_H
#define ENCORE_FAS_EVQ_H

#include <linux/fs.h>
#include <linux/poll.h>

#include "encore_fas_compat.h"
#include "uapi/encore_fas_uapi.h"

void fas_evq_init(void);
void fas_evq_push(const struct fas_event *ev);
void fas_evq_wake(void);
u32 fas_evq_dropped(void);

ssize_t fas_evq_read(struct file *file, char __user *buf, size_t count,
		     loff_t *ppos);
fas_poll_t fas_evq_poll(struct file *file, poll_table *wait);

#endif
