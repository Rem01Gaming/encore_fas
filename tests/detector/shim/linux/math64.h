#ifndef SHIM_LINUX_MATH64_H
#define SHIM_LINUX_MATH64_H
#include <linux/types.h>
static inline u64 div_u64(u64 a, u32 b) { return a / b; }
static inline u64 div64_u64(u64 a, u64 b) { return a / b; }
static inline s64 div64_s64(s64 a, s64 b) { return a / b; }
#endif
