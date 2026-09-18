#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/uprobes.h>
#include <trace/events/sched.h>

#include "encore_fas_context.h"
#include "encore_fas_eventq.h"
#include "encore_fas_compat.h"
#include "encore_fas_time.h"
#include "encore_fas_jank.h"

#define FAS_DEFAULT_FPS 60
#define FAS_SMALL_MULT 2
#define FAS_BIG_MULT 5
#define FAS_BIG_JANK_FLOOR_MS 200
#define FAS_EMA_DIVISOR 5
#define FAS_RELOCK_TOLERANCE_PCT 20
#define FAS_TARGET_TOLERANCE_PCT 10
#define FAS_EMA_CLAMP_MULT 2
#define FAS_WARMUP_CLEAN_SAMPLES 5
#define FAS_WARMUP_OUTLIER_MS 200

static DEFINE_MUTEX(fas_libgui_lock);
static u64 g_libgui_offset;
static char g_libgui_path[FAS_MAX_PATH_LEN];

static struct workqueue_struct *fas_teardown_wq;

static void fas_context_teardown(struct fas_context *ctx);

int fas_jank_set_libgui_offset(u64 offset, const char *path)
{
	mutex_lock(&fas_libgui_lock);
	g_libgui_offset = offset;
	strscpy(g_libgui_path, path, FAS_MAX_PATH_LEN);
	mutex_unlock(&fas_libgui_lock);

	return 0;
}

void fas_jank_get_libgui_offset(struct fas_libgui_offset *out)
{
	mutex_lock(&fas_libgui_lock);
	out->offset = g_libgui_offset;
	strscpy(out->path, g_libgui_path, FAS_MAX_PATH_LEN);
	mutex_unlock(&fas_libgui_lock);
}

static inline u64 fas_small_threshold(struct fas_context *ctx)
{
	return ctx->baseline_ms * FAS_SMALL_MULT;
}

static inline u64 fas_big_threshold(struct fas_context *ctx)
{
	return max_t(u64, ctx->baseline_ms * FAS_BIG_MULT,
		     FAS_BIG_JANK_FLOOR_MS);
}

static void fas_ema_update(struct fas_context *ctx, u64 delta)
{
	u64 ceiling = ctx->baseline_ms * FAS_EMA_CLAMP_MULT;
	s64 diff;

	if (ceiling && delta > ceiling)
		delta = ceiling;

	diff = (s64)delta - (s64)ctx->baseline_ms;
	ctx->baseline_ms =
		(u64)((s64)ctx->baseline_ms + diff / FAS_EMA_DIVISOR);
	if (ctx->baseline_ms == 0)
		ctx->baseline_ms = 1;
}

static bool fas_deltas_close(u64 a, u64 b, u32 pct)
{
	u64 diff = a > b ? a - b : b - a;
	u64 allowed = (a * pct) / 100;

	return diff <= allowed;
}

static void fas_relock_update(struct fas_context *ctx, u64 delta)
{
	if (ctx->relock_count > 0 &&
	    !fas_deltas_close(ctx->relock_deltas[ctx->relock_count - 1], delta,
			      FAS_RELOCK_TOLERANCE_PCT))
		ctx->relock_count = 0;

	ctx->relock_deltas[ctx->relock_count % FAS_RELOCK_STREAK] = delta;
	ctx->relock_count++;

	if (ctx->relock_count >= FAS_RELOCK_STREAK) {
		u64 sum = 0;
		int i;

		for (i = 0; i < FAS_RELOCK_STREAK; i++)
			sum += ctx->relock_deltas[i];

		ctx->baseline_ms = sum / FAS_RELOCK_STREAK;
		ctx->relock_count = 0;
	}
}

static u64 fas_nearest_target_ms(struct fas_context *ctx, u64 delta,
				 bool *matched)
{
	u64 best = ctx->target_frametimes_ms[0];
	u64 best_diff = delta > best ? delta - best : best - delta;
	u32 i;

	for (i = 1; i < ctx->target_count; i++) {
		u64 t = ctx->target_frametimes_ms[i];
		u64 diff = delta > t ? delta - t : t - delta;

		if (diff < best_diff) {
			best_diff = diff;
			best = t;
		}
	}

	*matched = best_diff <= (best * FAS_TARGET_TOLERANCE_PCT) / 100;
	return best;
}

static void fas_apply_fps_hint(struct fas_context *ctx, u32 fps)
{
	u64 ms = fps ? (1000 / fps) : (1000 / FAS_DEFAULT_FPS);

	ctx->baseline_ms = ms ? ms : 1;
}

static void fas_arm_watchdog(struct fas_context *ctx)
{
	ctx->watchdog_stage = 0;
	hrtimer_start(&ctx->watchdog_timer,
		      ms_to_ktime(fas_small_threshold(ctx)), HRTIMER_MODE_REL);
}

static bool fas_warmup_update(struct fas_context *ctx, u64 delta)
{
	if (delta < FAS_WARMUP_OUTLIER_MS) {
		ctx->warmup_sum_ms += delta;
		ctx->warmup_clean_count++;
	}

	if (ctx->warmup_clean_count < FAS_WARMUP_CLEAN_SAMPLES)
		return false;

	ctx->baseline_ms = ctx->warmup_sum_ms / ctx->warmup_clean_count;
	if (ctx->baseline_ms == 0)
		ctx->baseline_ms = 1;
	ctx->warmup_done = true;
	return true;
}

static void fas_median_push(struct fas_context *ctx, u64 delta)
{
	ctx->recent_deltas[ctx->recent_idx % FAS_MEDIAN_WINDOW] = delta;
	ctx->recent_idx++;
	if (ctx->recent_count < FAS_MEDIAN_WINDOW)
		ctx->recent_count++;
}

static u64 fas_median_delta(struct fas_context *ctx)
{
	u64 sorted[FAS_MEDIAN_WINDOW];
	u32 n = ctx->recent_count;
	u32 i, j;

	memcpy(sorted, ctx->recent_deltas, sizeof(u64) * n);

	for (i = 1; i < n; i++) {
		u64 key = sorted[i];

		j = i;
		while (j > 0 && sorted[j - 1] > key) {
			sorted[j] = sorted[j - 1];
			j--;
		}
		sorted[j] = key;
	}

	return sorted[n / 2];
}

static int fas_uprobe_handler(struct uprobe_consumer *self,
			      struct pt_regs *regs)
{
	struct fas_context *ctx =
		container_of(self, struct fas_context, consumer);
	u64 now, delta, sample;

	if (!atomic_read(&ctx->active))
		return 0;

	now = fas_now_ms();
	delta = now - ctx->last_frame_ms;
	ctx->last_frame_ms = now;

	hrtimer_cancel(&ctx->watchdog_timer);

	if (!ctx->target_count && !ctx->warmup_done) {
		fas_warmup_update(ctx, delta);
		fas_arm_watchdog(ctx);
		return 0;
	}

	if (ctx->target_count) {
		bool matched;
		u64 nearest = fas_nearest_target_ms(ctx, delta, &matched);

		if (matched)
			ctx->baseline_ms = nearest;
		else if (delta <= fas_big_threshold(ctx))
			fas_eventq_push(ctx->id, FAS_EVENT_SMALL_JANK, delta);
		else
			fas_eventq_push(ctx->id, FAS_EVENT_BIG_JANK, delta);

		fas_arm_watchdog(ctx);
		return 0;
	}

	fas_median_push(ctx, delta);
	sample = fas_median_delta(ctx);

	if (sample <= fas_small_threshold(ctx)) {
		fas_ema_update(ctx, sample);
		ctx->relock_count = 0;
	} else if (sample <= fas_big_threshold(ctx)) {
		fas_eventq_push(ctx->id, FAS_EVENT_SMALL_JANK, delta);
		fas_relock_update(ctx, sample);
	} else {
		fas_eventq_push(ctx->id, FAS_EVENT_BIG_JANK, delta);
		fas_relock_update(ctx, sample);
	}

	fas_arm_watchdog(ctx);
	return 0;
}

static bool fas_uprobe_filter(struct uprobe_consumer *self,
			      enum uprobe_filter_ctx uctx, struct mm_struct *mm)
{
	struct fas_context *ctx =
		container_of(self, struct fas_context, consumer);
	struct task_struct *task = pid_task(ctx->pid_struct, PIDTYPE_PID);

	return task && task->mm == mm;
}

static enum hrtimer_restart fas_watchdog_fn(struct hrtimer *timer)
{
	struct fas_context *ctx =
		container_of(timer, struct fas_context, watchdog_timer);
	u64 elapsed;

	if (!atomic_read(&ctx->active))
		return HRTIMER_NORESTART;

	elapsed = fas_now_ms() - ctx->last_frame_ms;

	if (ctx->watchdog_stage == 0) {
		fas_eventq_push(ctx->id, FAS_EVENT_BOOST_SOFT, elapsed);
		ctx->watchdog_stage = 1;
		hrtimer_forward_now(timer,
				    ms_to_ktime(fas_big_threshold(ctx) -
						fas_small_threshold(ctx)));
		return HRTIMER_RESTART;
	}

	fas_eventq_push(ctx->id, FAS_EVENT_BOOST_HARD, elapsed);
	ctx->watchdog_stage = 2;
	return HRTIMER_NORESTART;
}

static void fas_context_teardown(struct fas_context *ctx)
{
	if (atomic_cmpxchg(&ctx->active, 1, 0) != 1)
		return;

	if (ctx->uprobe_registered) {
		fas_uprobe_unregister(d_inode(ctx->libgui_path.dentry),
				      ctx->libgui_offset, &ctx->consumer);
		ctx->uprobe_registered = false;
	}

	hrtimer_cancel(&ctx->watchdog_timer);

	path_put(&ctx->libgui_path);

	fas_ctx_registry_remove(ctx);
	fas_ctx_put(ctx);
}

static void fas_teardown_work_fn(struct work_struct *work)
{
	struct fas_context *ctx =
		container_of(work, struct fas_context, teardown_work);

	fas_context_teardown(ctx);
	fas_ctx_put(ctx);
}

static void fas_schedule_teardown(struct fas_context *ctx)
{
	kref_get(&ctx->kref);
	queue_work(fas_teardown_wq, &ctx->teardown_work);
}

static void fas_process_exit_probe(void *data, struct task_struct *task)
{
	fas_ctx_for_each_matching_pid(task, fas_schedule_teardown);
}

long fas_jank_register_listener(pid_t pid, int *out_ctx_id)
{
	char path_snapshot[FAS_MAX_PATH_LEN];
	u64 offset_snapshot;
	struct fas_context *ctx;
	long ret;

	mutex_lock(&fas_libgui_lock);
	if (g_libgui_path[0] == '\0') {
		mutex_unlock(&fas_libgui_lock);
		return -ENOENT;
	}
	strscpy(path_snapshot, g_libgui_path, FAS_MAX_PATH_LEN);
	offset_snapshot = g_libgui_offset;
	mutex_unlock(&fas_libgui_lock);

	ctx = fas_ctx_alloc();
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->pid_struct = find_get_pid(pid);
	if (!ctx->pid_struct) {
		ret = -ESRCH;
		goto err_put_pid;
	}
	ctx->pid = pid;

	ret = kern_path(path_snapshot, LOOKUP_FOLLOW, &ctx->libgui_path);
	if (ret)
		goto err_put_pid;
	ctx->libgui_offset = offset_snapshot;

	ctx->last_frame_ms = fas_now_ms();
	ctx->watchdog_stage = 0;
	ctx->relock_count = 0;
	INIT_WORK(&ctx->teardown_work, fas_teardown_work_fn);
	fas_apply_fps_hint(ctx, FAS_DEFAULT_FPS);

	ctx->consumer.handler = fas_uprobe_handler;
	ctx->consumer.filter = fas_uprobe_filter;

	ret = uprobe_register(d_inode(ctx->libgui_path.dentry),
			      ctx->libgui_offset, &ctx->consumer);
	if (ret)
		goto err_put_path;
	ctx->uprobe_registered = true;

	fas_hrtimer_setup(&ctx->watchdog_timer, fas_watchdog_fn,
			  CLOCK_MONOTONIC, HRTIMER_MODE_REL);

	fas_ctx_publish(ctx);
	fas_arm_watchdog(ctx);

	*out_ctx_id = ctx->id;
	return 0;

err_put_path:
	path_put(&ctx->libgui_path);
err_put_pid:
	put_pid(ctx->pid_struct);
	fas_ctx_registry_remove(ctx);
	fas_ctx_put(ctx);
	return ret;
}

long fas_jank_remove_listener(int ctx_id)
{
	struct fas_context *ctx;

	ctx = fas_ctx_lookup_get(ctx_id);
	if (!ctx)
		return -ENOENT;

	fas_context_teardown(ctx);
	fas_ctx_put(ctx);
	return 0;
}

long fas_jank_hint_frametime(int ctx_id, u32 fps)
{
	struct fas_context *ctx;
	long ret = 0;

	ctx = fas_ctx_lookup_get(ctx_id);
	if (!ctx)
		return -ENOENT;

	if (!atomic_read(&ctx->active))
		ret = -ENOENT;
	else
		fas_apply_fps_hint(ctx, fps);

	fas_ctx_put(ctx);
	return ret;
}

long fas_jank_set_target_list(int ctx_id, const struct fas_target_fps_list *req)
{
	struct fas_context *ctx;
	u32 i;

	if (req->count > FAS_MAX_TARGETS)
		return -EINVAL;

	ctx = fas_ctx_lookup_get(ctx_id);
	if (!ctx)
		return -ENOENT;

	if (!atomic_read(&ctx->active)) {
		fas_ctx_put(ctx);
		return -ENOENT;
	}

	for (i = 0; i < req->count; i++) {
		if (req->fps[i] == 0 || req->fps[i] > 1000) {
			fas_ctx_put(ctx);
			return -EINVAL;
		}
		ctx->target_frametimes_ms[i] = 1000 / req->fps[i];
	}
	ctx->target_count = req->count;

	if (ctx->target_count) {
		bool matched;

		ctx->baseline_ms =
			fas_nearest_target_ms(ctx, ctx->baseline_ms, &matched);
	}

	fas_ctx_put(ctx);
	return 0;
}

int fas_jank_init(void)
{
	int ret;

	fas_teardown_wq = alloc_workqueue("encore_fas_teardown", WQ_UNBOUND, 0);
	if (!fas_teardown_wq)
		return -ENOMEM;

	ret = register_trace_sched_process_exit(fas_process_exit_probe, NULL);
	if (ret) {
		destroy_workqueue(fas_teardown_wq);
		return ret;
	}

	return 0;
}

void fas_jank_exit(void)
{
	unregister_trace_sched_process_exit(fas_process_exit_probe, NULL);
	tracepoint_synchronize_unregister();

	fas_ctx_teardown_all(fas_context_teardown);

	destroy_workqueue(fas_teardown_wq);
}
