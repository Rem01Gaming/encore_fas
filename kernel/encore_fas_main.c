#include <linux/init.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>

#include "encore_fas_context.h"
#include "encore_fas_eventq.h"
#include "encore_fas_jank.h"
#include "encore_fas_ioctl.h"
#include "encore_fas_util.h"

static int fas_open(struct inode *inode, struct file *file)
{
	if (!fas_caller_is_root())
		return -EPERM;

	return nonseekable_open(inode, file);
}

static const struct file_operations fas_fops = {
	.owner = THIS_MODULE,
	.open = fas_open,
	.unlocked_ioctl = fas_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = fas_ioctl_compat,
#endif
	.read = fas_eventq_read,
	.poll = fas_eventq_poll,
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

	pr_info("Encore FAS driver information:\n");
	pr_info("- FAS_VERSION = %u\n", FAS_VERSION);
	pr_info("- FAS_BRANCH  = %s\n", FAS_BRANCH);

	ret = fas_registry_init();
	if (ret)
		return ret;

	ret = fas_eventq_init();
	if (ret)
		goto err_registry;

	ret = fas_jank_init();
	if (ret)
		goto err_eventq;

	ret = misc_register(&fas_miscdev);
	if (ret)
		goto err_jank;

	return 0;

err_jank:
	fas_jank_exit();
err_eventq:
	fas_eventq_exit();
err_registry:
	fas_registry_exit();
	return ret;
}

static void __exit encore_fas_exit(void)
{
	misc_deregister(&fas_miscdev);
	fas_jank_exit();
	fas_eventq_exit();
	fas_registry_exit();
}

module_init(encore_fas_init);
module_exit(encore_fas_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Encore Frame Aware Scheduling module");
