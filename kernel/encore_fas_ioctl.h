#ifndef _ENCORE_FAS_IOCTL_H
#define _ENCORE_FAS_IOCTL_H

#include <linux/fs.h>

long fas_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

#ifdef CONFIG_COMPAT
long fas_ioctl_compat(struct file *file, unsigned int cmd, unsigned long arg);
#endif

#endif
