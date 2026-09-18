// SPDX-License-Identifier: GPL-2.0-only

#ifndef ENCORE_FAS_TIME_H
#define ENCORE_FAS_TIME_H

#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/time64.h>
#include <linux/types.h>

/**
 * @brief Constants of the counter.
 */
struct fas_clock {
	/** Counter frequency in hertz. */
	u64 freq;
	/** Multiplier of the tick to nanosecond conversion. */
	u64 mult;
	/** Largest input of fas_ticks_to_ns() that cannot overflow. */
	u64 max_ticks;
	/** Right shift of the tick to nanosecond conversion. */
	u32 shift;
};

extern struct fas_clock fas_clk;

/**
 * @brief Reads the virtual counter.
 *
 * @return The counter value in ticks.
 */
static __always_inline u64 fas_ticks(void)
{
	u64 val;

	asm volatile("mrs %0, cntvct_el0" : "=r"(val));
	return val;
}

/**
 * @brief Converts ticks to nanoseconds.
 *
 * @param ticks The value to convert. Larger values become fas_clk.max_ticks.
 * @return The value in nanoseconds.
 */
static __always_inline u64 fas_ticks_to_ns(u64 ticks)
{
	return (min(ticks, fas_clk.max_ticks) * fas_clk.mult) >> fas_clk.shift;
}

int fas_time_init(void);
u64 fas_ns_to_ticks(u64 ns);

#endif
