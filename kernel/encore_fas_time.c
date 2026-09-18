// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cache.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/errno.h>

#include "encore_fas_time.h"

static u64 fas_abs_diff(u64 a, u64 b)
{
	return a > b ? a - b : b - a;
}

struct fas_clock fas_clk __read_mostly;

#define FAS_MIN_FREQ 1000000ULL
#define FAS_MAX_FREQ 4000000000ULL
#define FAS_MAX_SECONDS 60

/**
 * @brief Reads the counter frequency and builds the conversion constants.
 *
 * @return 0 on success. Otherwise -ENODEV.
 */
int fas_time_init(void)
{
	u64 freq, measured, t0, t1, c0, c1;
	u32 shift;

	asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));

	t0 = ktime_get_ns();
	c0 = fas_ticks();
	usleep_range(4000, 5000);
	c1 = fas_ticks();
	t1 = ktime_get_ns();
	measured = div64_u64((c1 - c0) * NSEC_PER_SEC, t1 - t0);

	if (freq < FAS_MIN_FREQ || freq > FAS_MAX_FREQ ||
	    fas_abs_diff(freq, measured) > freq / 10) {
		pr_warn("encore_fas: CNTFRQ_EL0 is %llu Hz, measured %llu Hz\n", freq,
			measured);
		freq = round_up(measured, 10000);
	}

	if (freq < FAS_MIN_FREQ || freq > FAS_MAX_FREQ)
		return -ENODEV;

	fas_clk.freq = freq;
	fas_clk.max_ticks = freq * FAS_MAX_SECONDS;

	/*
	 * Find the largest shift for which max_ticks * mult still fits in 64
	 * bits. A larger shift gives a smaller rounding error.
	 */
	for (shift = 32; shift > 0; shift--) {
		u64 mult = div64_u64((u64)NSEC_PER_SEC << shift, freq);

		if (mult <= U64_MAX / fas_clk.max_ticks) {
			fas_clk.mult = mult;
			fas_clk.shift = shift;
			return 0;
		}
	}

	return -ENODEV;
}

/**
 * @brief Converts nanoseconds to ticks.
 *
 * @param ns The value to convert.
 * @return The value in ticks.
 */
u64 fas_ns_to_ticks(u64 ns)
{
	u32 rem;
	u64 sec = div_u64_rem(ns, NSEC_PER_SEC, &rem);

	return sec * fas_clk.freq + div_u64((u64)rem * fas_clk.freq, NSEC_PER_SEC);
}
