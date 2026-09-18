#ifndef SHIM_LINUX_COMPILER_H
#define SHIM_LINUX_COMPILER_H
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define noinline __attribute__((noinline))
#ifdef __always_inline
#undef __always_inline
#endif
#define __always_inline inline __attribute__((always_inline))
#endif
