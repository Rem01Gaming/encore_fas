// SPDX-License-Identifier: GPL-2.0-only

#ifndef ENCORE_FAS_DET_H
#define ENCORE_FAS_DET_H

#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/string.h>
#include <linux/types.h>

#include "uapi/encore_fas_uapi.h"

#define FAS_Q 16
#define FAS_Q_ONE (1LL << FAS_Q)

/* Tolerance of a deficit, and half width of a rate band, in percent. */
#define FAS_TOL_PCT 5
#define FAS_TOL_Q (FAS_Q_ONE * FAS_TOL_PCT / 100)

/* The CUSUM alarm limit and the clip of one interval, in periods. */
#define FAS_CUSUM_LIMIT_Q FAS_Q_ONE
#define FAS_CUSUM_CLIP_Q (FAS_Q_ONE / 2)
#define FAS_RECIP_SHIFT 48

/* A hitch that loses this many slots is big. */
#define FAS_MISS_BIG 3

/* Two rates must differ by this percent in period. */
#define FAS_MIN_RATIO_PCT 112

/* The margin is this many mean absolute deviations of the interval. */
#ifndef FAS_NOISE_MULT
#define FAS_NOISE_MULT 6
#endif

#define FAS_WIN_PERIODS 8
#define FAS_OK_WINDOWS 2
#define FAS_UP_WINDOWS 2
#define FAS_DOWN_WINDOWS 4
#define FAS_PAUSE_PERIODS 10

/* The idle poll period in units of the minimum window. It is 2 seconds. */
#define FAS_IDLE_POLL_WINS 8
#define FAS_NONE 0xffu
#define FAS_OUT_MAX 4

/* Watchdog stages. */
#define FAS_WD_ARMED 0
#define FAS_WD_SOFT_SENT 1
#define FAS_WD_HARD_SENT 2
#define FAS_WD_IDLE 3

/**
 * @brief One legal frame rate.
 */
struct fas_target {
	/** Ideal interval in ticks. */
	u32 period;
	/** Half width of the tolerance band in ticks. */
	u32 band;
	/** The frame rate. */
	u32 fps;
};

/**
 * @brief Read-mostly detector settings.
 */
struct fas_cfg {
	/** Period of the active target. */
	u64 period;
	/** 2^48 divided by @period. */
	u64 recip;
	/** Vsync period. */
	u64 vsync;
	/** A gap of this length is a pause. */
	u64 pause;
	/** Shortest window. It is 250 ms. */
	u64 win_min;
	/** Window length of the active target. */
	u64 win_need;
	/** Index of the active target. */
	u8 active;
	/** Number of targets. */
	u8 count;
	/** The detector must not switch to a slower target. */
	u8 lock_down;
	u8 pad[5];
	/** The targets, sorted from the fastest rate to the slowest rate. */
	struct fas_target tgt[FAS_MAX_TARGETS];
};

/**
 * @brief Detector state that each frame writes.
 */
struct fas_hot {
	/** Time of the last frame. */
	u64 last;
	/** Start time of the window. */
	u64 win_start;
	/** Moving average of the interval, times 16. */
	u64 cadence_q4;
	/** Moving mean absolute deviation of the interval, times 16. */
	u64 dev_q4;
	/** The CUSUM value in Q16. */
	s64 cusum_q;
	/** Intervals in the window. */
	u32 win_n;
	/** The detector has seen one frame. */
	u8 have_last;
	/** The detector has not chosen its first active target. */
	u8 acquiring;
	/** The game is slower than the active target. */
	u8 degraded;
	/** The watchdog reported a pause. */
	u8 paused;
	/** Watchdog stage, one of FAS_WD_*. */
	u8 wd_stage;
	/** Consecutive clean windows. */
	u8 ok_windows;
	/** Target that the last windows matched, or FAS_NONE. */
	u8 pending;
	/** Consecutive windows that matched @pending. */
	u8 pending_windows;
	/** Sequence number of the last event. */
	u32 seq;
};

/**
 * @brief One event that the detector wants to report.
 */
struct fas_ev {
	/** One of enum fas_event_type. */
	u32 type;
	/** FAS_EVF_* flags. */
	u32 flags;
	/** Missed slots. */
	u32 missed;
	/** Active target. */
	u32 fps;
	/** Interval or elapsed time in ticks. */
	u64 ticks;
};

/**
 * @brief Events that one detector call produces.
 */
struct fas_out {
	/** Number of events. */
	u32 n;
	/** The caller must check that the process is alive and poll again. */
	bool idle;
	/** The events. */
	struct fas_ev ev[FAS_OUT_MAX];
};

static __always_inline u64 fas_abs_diff(u64 a, u64 b)
{
	return a > b ? a - b : b - a;
}

/**
 * @brief Gets the reference interval.
 *
 * @param c The settings.
 * @param h The state.
 * @return The reference interval in ticks.
 */
static __always_inline u64 fas_ref(const struct fas_cfg *c,
				   const struct fas_hot *h)
{
	u64 p = c->period;

	return min_t(u64, max_t(u64, p, h->cadence_q4 >> 4), p << 1);
}

/**
 * @brief Gets the hitch margin.
 *
 * @param c The settings.
 * @param h The state.
 * @param ref The reference interval.
 * @return The margin in ticks.
 */
static __always_inline u64 fas_margin(const struct fas_cfg *c,
				      const struct fas_hot *h, u64 ref)
{
	u64 adaptive = (h->dev_q4 * FAS_NOISE_MULT) >> 4;

	return min_t(u64, max_t(u64, c->vsync >> 1, adaptive), ref);
}

/**
 * @brief Gets the time from a frame to the soft watchdog stage.
 *
 * @param c The settings.
 * @param h The state.
 * @return The delay in ticks.
 */
static __always_inline u64 fas_soft_ticks(const struct fas_cfg *c,
					  const struct fas_hot *h)
{
	u64 ref = fas_ref(c, h);

	return ref + fas_margin(c, h, ref);
}

/**
 * @brief Gets the time from a frame to the hard watchdog stage.
 *
 * @param c The settings.
 * @param h The state.
 * @return The delay in ticks.
 */
static __always_inline u64 fas_hard_ticks(const struct fas_cfg *c,
					  const struct fas_hot *h)
{
	u64 ref = fas_ref(c, h);

	return ref * FAS_MISS_BIG + fas_margin(c, h, ref);
}

/**
 * @brief Adds one event to the output.
 *
 * @param c The settings.
 * @param out The output.
 * @param type The event type.
 * @param flags The event flags.
 * @param missed Missed slots.
 * @param ticks Interval or elapsed time in ticks.
 */
static inline void fas_emit(const struct fas_cfg *c, struct fas_out *out,
			    u32 type, u32 flags, u32 missed, u64 ticks)
{
	struct fas_ev *ev;

	if (unlikely(out->n >= FAS_OUT_MAX))
		return;

	ev = &out->ev[out->n++];
	ev->type = type;
	ev->flags = flags;
	ev->missed = missed;
	ev->fps = c->tgt[c->active].fps;
	ev->ticks = ticks;
}

/**
 * @brief Makes a target the active target.
 *
 * @param c The settings.
 * @param idx Index of the target.
 */
static inline void fas_det_set_active(struct fas_cfg *c, u32 idx)
{
	c->active = idx;
	c->period = c->tgt[idx].period;
	c->recip = div64_u64(1ULL << FAS_RECIP_SHIFT, c->period);
	c->win_need = max_t(u64, c->win_min, c->period * FAS_WIN_PERIODS);
}

/**
 * @brief Validates a target list and resets the detector.
 *
 * The function changes @c and @h only when it succeeds.
 *
 * @param c The settings to fill.
 * @param h The state to reset.
 * @param freq Counter frequency in hertz.
 * @param fps The legal frame rates, in any order.
 * @param count Number of rates. The range is 1 to FAS_MAX_TARGETS.
 * @param vsync Vsync period in ticks. Zero to use the period of the fastest target.
 * @param lock_down True to forbid a switch to a slower target.
 * @return 0 on success. Otherwise -EINVAL.
 */
static int fas_det_setup(struct fas_cfg *c, struct fas_hot *h, u64 freq,
			 const u32 *fps, u32 count, u64 vsync, bool lock_down)
{
	struct fas_cfg tmp;
	u32 sorted[FAS_MAX_TARGETS];
	u32 i, j;

	if (!count || count > FAS_MAX_TARGETS)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		u32 key = fps[i];

		if (key == 0 || key > 1000)
			return -EINVAL;
		for (j = i; j > 0 && sorted[j - 1] < key; j--)
			sorted[j] = sorted[j - 1];
		sorted[j] = key;
	}

	memset(&tmp, 0, sizeof(tmp));
	for (i = 0; i < count; i++) {
		struct fas_target *t = &tmp.tgt[i];

		t->fps = sorted[i];
		t->period = (u32)div_u64(freq + (sorted[i] >> 1), sorted[i]);
		t->band = t->period * FAS_TOL_PCT / 100;
		if (i && (u64)t->period * 100 <
				 (u64)tmp.tgt[i - 1].period * FAS_MIN_RATIO_PCT)
			return -EINVAL;
	}

	if (vsync && (vsync < freq / 1000 || vsync > freq / 10))
		return -EINVAL;

	tmp.count = count;
	tmp.lock_down = lock_down;
	tmp.vsync = vsync ? vsync : tmp.tgt[0].period;
	tmp.win_min = freq / 4;
	tmp.pause = max_t(u64, freq,
			  (u64)tmp.tgt[count - 1].period * FAS_PAUSE_PERIODS);
	fas_det_set_active(&tmp, 0);

	*c = tmp;
	memset(h, 0, sizeof(*h));
	h->acquiring = 1;
	h->pending = FAS_NONE;
	h->wd_stage = FAS_WD_IDLE;
	return 0;
}

static __always_inline int fas_band_index(const struct fas_cfg *c, u64 mean)
{
	u32 i;

	for (i = 0; i < c->count; i++)
		if (fas_abs_diff(mean, c->tgt[i].period) <= c->tgt[i].band)
			return (int)i;
	return -1;
}

/**
 * @brief Switches the active target and clears the deficit state.
 *
 * @param h The state.
 * @param c The settings.
 * @param idx Index of the new target.
 * @param out The event output.
 */
static noinline void fas_det_switch(struct fas_hot *h, struct fas_cfg *c,
				    u32 idx, struct fas_out *out)
{
	fas_det_set_active(c, idx);
	h->cadence_q4 = c->period << 4;
	h->pending = FAS_NONE;
	h->pending_windows = 0;
	h->degraded = 0;
	h->ok_windows = 0;
	h->cusum_q = 0;
	fas_emit(c, out, FAS_EVENT_RATE_SWITCH, 0, 0, 0);
}

/**
 * @brief Chooses the first active target.
 *
 * @param h The state.
 * @param c The settings.
 * @param span Length of the acquisition window in ticks.
 * @param out The event output.
 */
static noinline void fas_det_acquire(struct fas_hot *h, struct fas_cfg *c,
				     u64 span, struct fas_out *out)
{
	u64 mean = div_u64(span, h->win_n);
	int k = fas_band_index(c, mean);
	u32 i;

	if (k < 0) {
		k = 0;
		for (i = 0; i < c->count; i++)
			if (c->tgt[i].period <= mean)
				k = (int)i;
	}

	h->acquiring = 0;
	h->wd_stage = FAS_WD_ARMED;
	fas_det_switch(h, c, (u32)k, out);
	h->win_start = h->last;
	h->win_n = 0;
}

/**
 * @brief Counts windows that match one other target.
 *
 * @param h The state.
 * @param c The settings.
 * @param mean The mean interval of the closed window.
 * @param out The event output.
 */
static noinline void fas_det_rate_track(struct fas_hot *h, struct fas_cfg *c,
					u64 mean, struct fas_out *out)
{
	int k = fas_band_index(c, mean);
	bool slower;
	u32 need;

	if (k < 0 || (u32)k == c->active) {
		h->pending = FAS_NONE;
		h->pending_windows = 0;
		return;
	}

	if ((u32)k != h->pending) {
		h->pending = (u8)k;
		h->pending_windows = 0;
	}
	if (h->pending_windows < 255)
		h->pending_windows++;

	slower = c->tgt[k].period > c->period;
	if (slower && c->lock_down)
		return;

	need = slower ? FAS_DOWN_WINDOWS : FAS_UP_WINDOWS;
	if (h->pending_windows >= need)
		fas_det_switch(h, c, (u32)k, out);
}

/**
 * @brief Applies the decisions of a closed window.
 *
 * @param h The state.
 * @param c The settings.
 * @param now Time of the frame that closes the window.
 * @param out The event output.
 */
static noinline void fas_det_window_close(struct fas_hot *h, struct fas_cfg *c,
					  u64 now, struct fas_out *out)
{
	u64 span = now - h->win_start;
	u64 mean = div_u64(span, h->win_n);

	if (h->degraded) {
		u64 rho_q = div64_u64(span << FAS_Q, (u64)h->win_n * c->period);

		if (rho_q <= (u64)(FAS_Q_ONE + FAS_TOL_Q)) {
			if (++h->ok_windows >= FAS_OK_WINDOWS) {
				h->degraded = 0;
				h->ok_windows = 0;
				h->cusum_q = 0;
				fas_emit(c, out, FAS_EVENT_RECOVERED, 0, 0, mean);
			}
		} else {
			h->ok_windows = 0;
		}
	}

	fas_det_rate_track(h, c, mean, out);
	h->win_start = now;
	h->win_n = 0;
}

/**
 * @brief Reports a hitch.
 *
 * @param h The state.
 * @param c The settings.
 * @param delta The interval in ticks.
 * @param ref The reference interval.
 * @param margin The margin.
 * @param flags Event flags.
 * @param out The event output.
 */
static noinline void fas_det_hitch(const struct fas_hot *h,
				   const struct fas_cfg *c, u64 delta, u64 ref,
				   u64 margin, u32 flags, struct fas_out *out)
{
	u64 missed = div64_u64(delta - margin, ref);
	u32 type = missed >= FAS_MISS_BIG ? FAS_EVENT_BIG_JANK :
					    FAS_EVENT_SMALL_JANK;

	if (h->degraded && type != FAS_EVENT_BIG_JANK)
		return;

	fas_emit(c, out, type, flags, (u32)min_t(u64, missed, 0xffff), delta);
}

/**
 * @brief Handles the first frame after a pause.
 *
 * @param h The state.
 * @param c The settings.
 * @param now Time of the frame.
 * @param out The event output.
 */
static noinline void fas_det_resync(struct fas_hot *h, const struct fas_cfg *c,
				    u64 now, struct fas_out *out)
{
	if (h->paused)
		fas_emit(c, out, FAS_EVENT_RESUMED, 0, 0, 0);

	h->paused = 0;
	h->degraded = 0;
	h->ok_windows = 0;
	h->cusum_q = 0;
	h->pending = FAS_NONE;
	h->pending_windows = 0;
	h->win_start = now;
	h->win_n = 0;
	if (!h->acquiring) {
		h->wd_stage = FAS_WD_ARMED;
		h->cadence_q4 = c->period << 4;
	}
}

/**
 * @brief Updates the cadence and the deviation.
 *
 * @param h The state.
 * @param c The settings.
 * @param delta An interval that is not a hitch.
 */
static __always_inline void fas_det_noise_update(struct fas_hot *h,
						 const struct fas_cfg *c,
						 u64 delta)
{
	u64 dev = min_t(u64, fas_abs_diff(delta, h->cadence_q4 >> 4), c->vsync);

	h->dev_q4 = (u64)((s64)h->dev_q4 +
			  ((((s64)dev << 4) - (s64)h->dev_q4) >> 4));
	h->cadence_q4 = (u64)((s64)h->cadence_q4 +
			      ((((s64)delta << 4) - (s64)h->cadence_q4) >> 3));
}

/**
 * @brief Adds one interval to the one-sided CUSUM.
 *
 * @param h The state.
 * @param c The settings.
 * @param delta The interval in ticks.
 * @return true when the CUSUM reaches the alarm limit.
 */
static __always_inline bool fas_det_cusum(struct fas_hot *h,
					  const struct fas_cfg *c, u64 delta)
{
	u64 p = c->period;
	s64 x = ((s64)min_t(u64, delta, p + (p >> 1)) - (s64)p) *
			(s64)c->recip >> (FAS_RECIP_SHIFT - FAS_Q);
	s64 s = h->cusum_q + x - FAS_TOL_Q;

	h->cusum_q = s > 0 ? s : 0;
	return unlikely(h->cusum_q >= FAS_CUSUM_LIMIT_Q);
}

/**
 * @brief Processes one frame.
 *
 * @param h The state.
 * @param c The settings.
 * @param now Time of the frame in ticks.
 * @param out The event output. The function sets @out->n and @out->idle.
 * @return The delay to the first watchdog stage in ticks. Zero means that the
 *         caller must leave the watchdog timer as it is.
 */
static u64 fas_det_frame(struct fas_hot *h, struct fas_cfg *c, u64 now,
			 struct fas_out *out)
{
	u64 delta, ref, margin;
	u32 flags;

	out->n = 0;
	out->idle = false;

	if (unlikely(!h->have_last)) {
		h->have_last = 1;
		h->acquiring = 1;
		h->last = now;
		h->win_start = now;
		h->win_n = 0;
		return 0;
	}

	/*
	 * Two handlers on different CPUs can read the counter in the opposite
	 * order of their lock order. Treat such a frame as a duplicate.
	 */
	delta = now - h->last;
	if (unlikely((s64)delta <= 0))
		return 0;
	h->last = now;

	if (unlikely(h->paused || delta >= c->pause)) {
		fas_det_resync(h, c, now, out);
		return h->acquiring ? 0 : fas_soft_ticks(c, h);
	}

	if (unlikely(h->acquiring)) {
		h->win_n++;
		if (now - h->win_start < c->win_min)
			return 0;
		fas_det_acquire(h, c, now - h->win_start, out);
		return fas_soft_ticks(c, h);
	}

	flags = h->wd_stage ? FAS_EVF_WATCHDOG : 0;
	h->wd_stage = FAS_WD_ARMED;

	ref = fas_ref(c, h);
	margin = fas_margin(c, h, ref);
	if (unlikely(delta >= ref + margin))
		fas_det_hitch(h, c, delta, ref, margin, flags, out);
	else
		fas_det_noise_update(h, c, delta);

	if (likely(!h->degraded) && fas_det_cusum(h, c, delta)) {
		fas_emit(c, out, FAS_EVENT_DEGRADED, 0, 0, delta);
		h->degraded = 1;
		h->cusum_q = 0;
	}

	h->win_n++;
	if (unlikely(now - h->win_start >= c->win_need))
		fas_det_window_close(h, c, now, out);

	return fas_soft_ticks(c, h);
}

/**
 * @brief Runs one watchdog stage. Call it from the timer callback.
 *
 * @param h The state.
 * @param c The settings.
 * @param now Current time in ticks.
 * @param out The event output. The function sets @out->n and @out->idle.
 * @return The delay to the next stage in ticks.
 */
static u64 fas_det_wd_fire(struct fas_hot *h, const struct fas_cfg *c, u64 now,
			   struct fas_out *out)
{
	u64 elapsed = (s64)(now - h->last) > 0 ? now - h->last : 0;
	u64 limit;

	out->n = 0;
	out->idle = false;

	switch (h->wd_stage) {
	case FAS_WD_ARMED:
    	limit = fas_soft_ticks(c, h);
    	if (elapsed < limit)
        	return 0;
    	h->wd_stage = FAS_WD_SOFT_SENT;
    	fas_emit(c, out, FAS_EVENT_BOOST_SOFT, 0, 0, elapsed);
    	limit = fas_hard_ticks(c, h);
    	return limit > elapsed ? limit - elapsed : 1;
	case FAS_WD_SOFT_SENT:
		limit = fas_hard_ticks(c, h);
		if (elapsed < limit)
			return limit - elapsed;
		h->wd_stage = FAS_WD_HARD_SENT;
		fas_emit(c, out, FAS_EVENT_BOOST_HARD, 0, 0, elapsed);
		return c->pause > elapsed ? c->pause - elapsed : 1;
	case FAS_WD_HARD_SENT:
		if (elapsed < c->pause)
			return c->pause - elapsed;
		h->wd_stage = FAS_WD_IDLE;
		h->paused = 1;
		fas_emit(c, out, FAS_EVENT_PAUSED, 0, 0, elapsed);
		break;
	default:
		break;
	}

	out->idle = true;
	return c->win_min * FAS_IDLE_POLL_WINS;
}

/**
 * @brief Gets the deficit accumulator for events and state.
 *
 * @param h The state.
 * @return The CUSUM value in Q16, at most 65536.
 */
static __always_inline u32 fas_det_pressure(const struct fas_hot *h)
{
	return (u32)min_t(s64, h->cusum_q, FAS_Q_ONE);
}

#endif
