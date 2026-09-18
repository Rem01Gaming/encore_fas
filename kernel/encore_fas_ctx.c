// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/pid.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/spinlock.h>
#include <linux/uprobes.h>
#include <linux/workqueue.h>

#include "encore_fas_compat.h"
#include "encore_fas_ctx.h"
#include "encore_fas_det.h"
#include "encore_fas_evq.h"
#include "encore_fas_time.h"

struct fas_ctx {
	raw_spinlock_t lock ____cacheline_aligned;
	/** Thread group ID of the game. The handler compares it to current. */
	u32 tgid;
	/** Detector state. The lock, @tgid and this member fill one cache line. */
	struct fas_hot hot;

	/** Listener ID that the daemon uses. */
	u32 id ____cacheline_aligned;
	/** Detector settings. The first 56 bytes share a line with @id. */
	struct fas_cfg cfg;

	struct hrtimer wd ____cacheline_aligned;
	/** The uprobe consumer. */
	struct uprobe_consumer uc;
	/** The uprobe handle. */
	struct fas_probe probe;
	/** Reference to the probed file. */
	struct path path;
	/** Reference to the thread group leader. */
	struct pid *pid;
	/** Cleanup work for a dead process. */
	struct work_struct work;
	/** The slot holds a listener. Only fas_mutex changes it. */
	bool in_use;
};

static struct fas_ctx fas_slots[FAS_MAX_LISTENERS];
static DEFINE_MUTEX(fas_mutex);
static u32 fas_next_id;

/**
 * @brief Checks that the game still runs.
 *
 * @param ctx The listener.
 * @return true when the process is alive.
 */
static bool fas_ctx_alive(const struct fas_ctx *ctx)
{
	struct task_struct *task;
	struct signal_struct *sig;
	bool alive;

	rcu_read_lock();
	task = pid_task(ctx->pid, PIDTYPE_PID);
	sig = task ? READ_ONCE(task->signal) : NULL;
	alive = task && (!READ_ONCE(task->exit_state) ||
			 (sig && atomic_read(&sig->live) > 0));
	rcu_read_unlock();

	return alive;
}

/**
 * @brief Starts the watchdog timer.
 *
 * @param ctx The listener.
 * @param ticks The delay in ticks.
 */
static void fas_ctx_arm(struct fas_ctx *ctx, u64 ticks)
{
	u64 ns = fas_ticks_to_ns(ticks);

	hrtimer_start_range_ns(&ctx->wd, ns_to_ktime(ns), ns >> 4,
			       FAS_TIMER_MODE);
}

/**
 * @brief Converts detector events and queues them.
 *
 * @param ctx The listener. The caller holds ctx->lock.
 * @param out The events from the detector.
 */
static void fas_ctx_publish(struct fas_ctx *ctx, const struct fas_out *out)
{
	u64 ts = ktime_get_ns();
	u32 pressure = fas_det_pressure(&ctx->hot);
	u32 i;

	for (i = 0; i < out->n; i++) {
		const struct fas_ev *e = &out->ev[i];
		struct fas_event ev = {
			.ctx_id = (s32)ctx->id,
			.type = e->type,
			.timestamp_ns = ts,
			.frametime_ns = fas_ticks_to_ns(e->ticks),
			.fps = e->fps,
			.missed = e->missed,
			.flags = e->flags,
			.pressure_q16 = pressure,
			.seq = ++ctx->hot.seq,
		};

		fas_evq_push(&ev);
	}
}

/**
 * @brief Runs at each call of Surface::queueBuffer.
 *
 * @return Always 0.
 */
static int fas_uprobe_handler(FAS_UPROBE_HANDLER_ARGS)
{
	struct fas_ctx *ctx = container_of(uc, struct fas_ctx, uc);
	struct fas_out out;
	unsigned long flags;
	u64 delay;

	if (unlikely(current->tgid != ctx->tgid))
		return 0;

	raw_spin_lock_irqsave(&ctx->lock, flags);
	delay = fas_det_frame(&ctx->hot, &ctx->cfg, fas_ticks(), &out);
	if (unlikely(out.n))
		fas_ctx_publish(ctx, &out);
	if (delay)
		fas_ctx_arm(ctx, delay);
	raw_spin_unlock_irqrestore(&ctx->lock, flags);

	if (unlikely(out.n))
		fas_evq_wake();

	return 0;
}

/**
 * @brief Tells the uprobe core where to insert the breakpoint.
 *
 * @return true when @mm belongs to the game.
 */
static bool fas_uprobe_filter(FAS_UPROBE_FILTER_ARGS)
{
	const struct fas_ctx *ctx = container_of(uc, struct fas_ctx, uc);
	struct task_struct *task;
	bool match;

	rcu_read_lock();
	task = pid_task(ctx->pid, PIDTYPE_PID);
	match = task && task->mm == mm;
	rcu_read_unlock();

	return match;
}

/**
 * @brief Watchdog timer callback.
 *
 * @return Always HRTIMER_NORESTART.
 */
static enum hrtimer_restart fas_wd_fn(struct hrtimer *timer)
{
	struct fas_ctx *ctx = container_of(timer, struct fas_ctx, wd);
	struct fas_out out;
	unsigned long flags;
	u64 delay;

	raw_spin_lock_irqsave(&ctx->lock, flags);
	delay = fas_det_wd_fire(&ctx->hot, &ctx->cfg, fas_ticks(), &out);
	if (out.n)
		fas_ctx_publish(ctx, &out);
	if (out.idle && !fas_ctx_alive(ctx)) {
		queue_work(system_unbound_wq, &ctx->work);
		delay = 0;
	}
	if (delay)
		fas_ctx_arm(ctx, delay);
	raw_spin_unlock_irqrestore(&ctx->lock, flags);

	if (out.n)
		fas_evq_wake();

	return HRTIMER_NORESTART;
}

/**
 * @brief Detaches a listener.
 *
 * @param ctx The listener.
 */
static void fas_ctx_teardown(struct fas_ctx *ctx)
{
	if (!ctx->in_use)
		return;

	fas_uprobe_unregister(&ctx->probe, &ctx->uc);
	hrtimer_cancel(&ctx->wd);
	path_put(&ctx->path);
	put_pid(ctx->pid);
	ctx->pid = NULL;
	ctx->in_use = false;
}

static void fas_ctx_work_fn(struct work_struct *work)
{
	struct fas_ctx *ctx = container_of(work, struct fas_ctx, work);

	mutex_lock(&fas_mutex);
	if (ctx->in_use && !fas_ctx_alive(ctx))
		fas_ctx_teardown(ctx);
	mutex_unlock(&fas_mutex);
}

static struct fas_ctx *fas_ctx_find(s32 ctx_id)
{
	u32 i;

	for (i = 0; i < FAS_MAX_LISTENERS; i++)
		if (fas_slots[i].in_use && fas_slots[i].id == (u32)ctx_id)
			return &fas_slots[i];
	return NULL;
}

static int fas_cfg_build(struct fas_cfg *c, struct fas_hot *h,
			 const struct fas_config *cfg)
{
	u64 vsync = cfg->vsync_ns ? fas_ns_to_ticks(cfg->vsync_ns) : 0;

	if (cfg->flags & ~FAS_CFG_LOCK_DOWN)
		return -EINVAL;

	return fas_det_setup(c, h, fas_clk.freq, cfg->fps, cfg->count, vsync,
			     cfg->flags & FAS_CFG_LOCK_DOWN);
}

/**
 * @brief Prepares the slots.
 */
void fas_ctx_init(void)
{
	u32 i;

	BUILD_BUG_ON(sizeof(struct fas_register_args) != 320);
	BUILD_BUG_ON(sizeof(struct fas_config) != 48);
	BUILD_BUG_ON(sizeof(struct fas_hot) != 56);
#if L1_CACHE_BYTES == 64 && !defined(CONFIG_DEBUG_SPINLOCK) && \
	!defined(CONFIG_LOCKDEP)
	/* The lock, the process ID and the hot state fill one line. */
	BUILD_BUG_ON(offsetof(struct fas_ctx, id) != L1_CACHE_BYTES);
#endif

	for (i = 0; i < FAS_MAX_LISTENERS; i++)
		INIT_WORK(&fas_slots[i].work, fas_ctx_work_fn);
}

/**
 * @brief Detaches all listeners.
 */
void fas_ctx_exit(void)
{
	u32 i;

	mutex_lock(&fas_mutex);
	for (i = 0; i < FAS_MAX_LISTENERS; i++)
		fas_ctx_teardown(&fas_slots[i]);
	mutex_unlock(&fas_mutex);

	/* A queued work item can still wait for fas_mutex. Wait for it. */
	for (i = 0; i < FAS_MAX_LISTENERS; i++)
		cancel_work_sync(&fas_slots[i].work);
}

/**
 * @brief Attaches a probe to a game process.
 *
 * @param req The request. The function sets @req->ctx_id.
 * @return 0 on success. Otherwise a negative error code.
 */
int fas_ctx_register(struct fas_register_args *req)
{
	struct fas_ctx *ctx = NULL;
	struct task_struct *task;
	struct inode *inode;
	struct pid *pid = NULL;
	u32 tgid = 0;
	u32 i;
	int ret;

	req->path[FAS_MAX_PATH_LEN - 1] = '\0';

	rcu_read_lock();
	task = pid_task(find_vpid(req->pid), PIDTYPE_PID);
	if (task) {
		tgid = task->tgid;
		pid = get_pid(task_tgid(task));
	}
	rcu_read_unlock();
	if (!pid)
		return -ESRCH;

	mutex_lock(&fas_mutex);

	for (i = 0; i < FAS_MAX_LISTENERS; i++) {
		if (fas_slots[i].in_use && fas_slots[i].tgid == tgid) {
			ret = -EEXIST;
			goto err_pid;
		}
		if (!ctx && !fas_slots[i].in_use)
			ctx = &fas_slots[i];
	}
	if (!ctx) {
		ret = -ENOSPC;
		goto err_pid;
	}

	ret = fas_cfg_build(&ctx->cfg, &ctx->hot, &req->cfg);
	if (ret)
		goto err_pid;

	ret = kern_path(req->path, LOOKUP_FOLLOW, &ctx->path);
	if (ret)
		goto err_pid;

	inode = d_inode(ctx->path.dentry);
	if (!S_ISREG(inode->i_mode) || (req->offset & 3) ||
	    req->offset >= (u64)i_size_read(inode)) {
		ret = -EINVAL;
		goto err_path;
	}

	raw_spin_lock_init(&ctx->lock);
	ctx->tgid = tgid;
	ctx->pid = pid;
	ctx->id = ++fas_next_id & INT_MAX;
	if (!ctx->id)
		ctx->id = (++fas_next_id) & INT_MAX;
	memset(&ctx->uc, 0, sizeof(ctx->uc));
	ctx->uc.handler = fas_uprobe_handler;
	ctx->uc.filter = fas_uprobe_filter;
	fas_hrtimer_setup(&ctx->wd, fas_wd_fn, CLOCK_MONOTONIC, FAS_TIMER_MODE);
	fas_ctx_arm(ctx, ctx->cfg.win_min * FAS_IDLE_POLL_WINS);

	ret = fas_uprobe_register(&ctx->probe, inode, req->offset, &ctx->uc);
	if (ret) {
		hrtimer_cancel(&ctx->wd);
		goto err_path;
	}

	ctx->in_use = true;
	req->ctx_id = (s32)ctx->id;
	mutex_unlock(&fas_mutex);
	return 0;

err_path:
	path_put(&ctx->path);
err_pid:
	put_pid(pid);
	mutex_unlock(&fas_mutex);
	return ret;
}

/**
 * @brief Detaches a listener.
 *
 * @param ctx_id The listener ID.
 * @return 0 on success. Otherwise -ENOENT.
 */
int fas_ctx_remove(s32 ctx_id)
{
	struct fas_ctx *ctx;
	int ret = -ENOENT;

	mutex_lock(&fas_mutex);
	ctx = fas_ctx_find(ctx_id);
	if (ctx) {
		fas_ctx_teardown(ctx);
		ret = 0;
	}
	mutex_unlock(&fas_mutex);

	return ret;
}

/**
 * @brief Replaces the targets of a listener.
 *
 * @param ctx_id The listener ID.
 * @param cfg The new targets.
 * @return 0 on success. Otherwise a negative error code.
 */
int fas_ctx_set_config(s32 ctx_id, const struct fas_config *cfg)
{
	struct fas_ctx *ctx;
	struct fas_cfg c;
	struct fas_hot h;
	unsigned long flags;
	int ret;

	ret = fas_cfg_build(&c, &h, cfg);
	if (ret)
		return ret;

	mutex_lock(&fas_mutex);
	ctx = fas_ctx_find(ctx_id);
	if (!ctx) {
		mutex_unlock(&fas_mutex);
		return -ENOENT;
	}

	raw_spin_lock_irqsave(&ctx->lock, flags);
	ctx->cfg = c;
	h.seq = ctx->hot.seq;
	ctx->hot = h;
	fas_ctx_arm(ctx, c.win_min * FAS_IDLE_POLL_WINS);
	raw_spin_unlock_irqrestore(&ctx->lock, flags);
	mutex_unlock(&fas_mutex);

	return 0;
}

/**
 * @brief Reads the state of a listener.
 *
 * @return 0 on success. Otherwise -ENOENT.
 */
int fas_ctx_get_state(struct fas_state *state)
{
	struct fas_ctx *ctx;
	unsigned long flags;
	s32 id = state->ctx_id;

	memset(state, 0, sizeof(*state));
	state->ctx_id = id;

	mutex_lock(&fas_mutex);
	ctx = fas_ctx_find(id);
	if (!ctx) {
		mutex_unlock(&fas_mutex);
		return -ENOENT;
	}

	raw_spin_lock_irqsave(&ctx->lock, flags);
	state->fps = ctx->cfg.tgt[ctx->cfg.active].fps;
	state->flags = (ctx->hot.acquiring ? FAS_STATE_ACQUIRING : 0) |
		       (ctx->hot.degraded ? FAS_STATE_DEGRADED : 0) |
		       (ctx->hot.paused ? FAS_STATE_PAUSED : 0);
	state->pressure_q16 = fas_det_pressure(&ctx->hot);
	state->seq = ctx->hot.seq;
	raw_spin_unlock_irqrestore(&ctx->lock, flags);
	mutex_unlock(&fas_mutex);

	state->dropped = fas_evq_dropped();
	return 0;
}

/**
 * @brief Lists the attached listeners.
 *
 * @param list The structure that receives the list.
 */
void fas_ctx_list(struct fas_listener_list *list)
{
	u32 i;

	memset(list, 0, sizeof(*list));

	mutex_lock(&fas_mutex);
	for (i = 0; i < FAS_MAX_LISTENERS; i++) {
		struct fas_ctx *ctx = &fas_slots[i];

		if (!ctx->in_use)
			continue;
		list->listeners[list->count].ctx_id = (s32)ctx->id;
		list->listeners[list->count].pid = pid_vnr(ctx->pid);
		list->count++;
	}
	mutex_unlock(&fas_mutex);
}
