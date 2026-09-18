#ifndef _ENCORE_FAS_TIME_H
#define _ENCORE_FAS_TIME_H

#include <linux/ktime.h>
#include <linux/types.h>

static inline u64 fas_now_ms(void)
{
	return ktime_to_ms(ktime_get());
}

#endif
