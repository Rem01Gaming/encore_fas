#ifndef SHIM_LINUX_KERNEL_H
#define SHIM_LINUX_KERNEL_H
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b) ((t)(a) > (t)(b) ? (t)(a) : (t)(b))
#endif
