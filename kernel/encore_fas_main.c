// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "encore_fas_compat.h"
#include "encore_fas_ctx.h"
#include "encore_fas_evq.h"
#include "encore_fas_time.h"

#ifndef FAS_VERSION
#define FAS_VERSION 0
#endif
#ifndef FAS_BRANCH
#define FAS_BRANCH "-"
#endif

static bool fas_caller_is_root(void)
{
	return uid_eq(current_euid(), GLOBAL_ROOT_UID);
}

static int fas_open(struct inode *inode, struct file *file)
{
	if (!fas_caller_is_root())
		return -EPERM;

	return nonseekable_open(inode, file);
}

static long fas_ioctl_register(void __user *uarg)
{
	struct fas_register_args req;
	long ret;

	if (copy_from_user(&req, uarg, sizeof(req)))
		return -EFAULT;

	ret = fas_ctx_register(&req);
	if (ret)
		return ret;

	if (put_user(req.ctx_id,
		     &((struct fas_register_args __user *)uarg)->ctx_id)) {
		fas_ctx_remove(req.ctx_id);
		return -EFAULT;
	}

	return 0;
}

static long fas_ioctl_get_state(void __user *uarg)
{
	struct fas_state state;
	long ret;

	if (copy_from_user(&state, uarg, sizeof(state)))
		return -EFAULT;

	ret = fas_ctx_get_state(&state);
	if (ret)
		return ret;

	return copy_to_user(uarg, &state, sizeof(state)) ? -EFAULT : 0;
}

/**
 * @brief Handles the control commands. Only root can call it.
 *
 * @return 0 on success, or a negative error code.
 */
static long fas_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	if (!fas_caller_is_root())
		return -EPERM;

	if (_IOC_TYPE(cmd) != FAS_IOC_MAGIC || _IOC_NR(cmd) > FAS_IOC_MAXNR)
		return -ENOTTY;

	switch (cmd) {
	case FAS_IOC_GET_VERSION: {
		struct fas_version ver = {
			.version = FAS_VERSION,
			.abi = FAS_ABI_VERSION,
			.counter_hz = (u32)fas_clk.freq,
		};

		return copy_to_user(uarg, &ver, sizeof(ver)) ? -EFAULT : 0;
	}
	case FAS_IOC_REGISTER:
		return fas_ioctl_register(uarg);
	case FAS_IOC_REMOVE: {
		struct fas_remove_args req;

		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;
		return fas_ctx_remove(req.ctx_id);
	}
	case FAS_IOC_SET_CONFIG: {
		struct fas_config_args req;

		if (copy_from_user(&req, uarg, sizeof(req)))
			return -EFAULT;
		return fas_ctx_set_config(req.ctx_id, &req.cfg);
	}
	case FAS_IOC_GET_STATE:
		return fas_ioctl_get_state(uarg);
	case FAS_IOC_LIST: {
		struct fas_listener_list list;

		fas_ctx_list(&list);
		return copy_to_user(uarg, &list, sizeof(list)) ? -EFAULT : 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations fas_fops = {
	.owner = THIS_MODULE,
	.open = fas_open,
	.unlocked_ioctl = fas_ioctl,
	.read = fas_evq_read,
	.poll = fas_evq_poll,
};

static struct miscdevice fas_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "encore_fas",
	.fops = &fas_fops,
	.mode = 0600,
};

static int __init encore_fas_init(void)
{
	int ret;

	ret = fas_time_init();
	if (ret) {
		pr_err("encore_fas: the counter frequency is not usable\n");
		return ret;
	}

	fas_evq_init();
	fas_ctx_init();

	ret = misc_register(&fas_miscdev);
	if (ret)
		return ret;

	pr_info("encore_fas: version %u, branch %s, counter %llu Hz\n",
		FAS_VERSION, FAS_BRANCH, fas_clk.freq);
	return 0;
}

static void __exit encore_fas_exit(void)
{
	misc_deregister(&fas_miscdev);
	fas_ctx_exit();
}

module_init(encore_fas_init);
module_exit(encore_fas_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Encore Frame Aware Scheduling module");
