#ifndef _ENCORE_FAS_CONTEXT_H
#define _ENCORE_FAS_CONTEXT_H

#include <linux/types.h>
#include <linux/kref.h>
#include <linux/atomic.h>
#include <linux/hrtimer.h>
#include <linux/uprobes.h>
#include <linux/workqueue.h>
#include <linux/list.h>
#include <linux/path.h>
#include <linux/pid.h>
#include <linux/sched.h>

#include "uapi/encore_fas_uapi.h"

#define FAS_RELOCK_STREAK 3
#define FAS_MEDIAN_WINDOW 5

struct fas_context {
	struct kref kref;
	atomic_t active;
	int id;

	struct pid *pid_struct;
	pid_t pid;

	struct path libgui_path;
	u64 libgui_offset;
	bool uprobe_registered;
	struct uprobe_consumer consumer;

	struct hrtimer watchdog_timer;
	u64 baseline_ms;
	u64 last_frame_ms;
	int watchdog_stage;
	u32 relock_count;
	u64 relock_deltas[FAS_RELOCK_STREAK];

	u64 warmup_sum_ms;
	u32 warmup_clean_count;
	bool warmup_done;

	u64 recent_deltas[FAS_MEDIAN_WINDOW];
	u32 recent_count;
	u32 recent_idx;

	u32 target_count;
	u64 target_frametimes_ms[FAS_MAX_TARGETS];

	struct work_struct teardown_work;
	struct list_head node;
};

int fas_registry_init(void);
void fas_registry_exit(void);

struct fas_context *fas_ctx_alloc(void);
void fas_ctx_publish(struct fas_context *ctx);
void fas_ctx_registry_remove(struct fas_context *ctx);

struct fas_context *fas_ctx_lookup_get(int ctx_id);
void fas_ctx_put(struct fas_context *ctx);

void fas_ctx_for_each_matching_pid(struct task_struct *task,
				   void (*fn)(struct fas_context *ctx));

void fas_ctx_teardown_all(void (*teardown_fn)(struct fas_context *ctx));

#endif
