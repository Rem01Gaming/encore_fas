#include <linux/uaccess.h>
#include <linux/compat.h>

#include "uapi/encore_fas_uapi.h"

#include "encore_fas_ioctl.h"
#include "encore_fas_jank.h"
#include "encore_fas_util.h"

static long fas_ioctl_get_version(void __user *arg)
{
	struct fas_version resp = {
		.version = FAS_VERSION,
	};

	if (copy_to_user(arg, &resp, sizeof(resp)))
		return -EFAULT;

	return 0;
}

static long fas_ioctl_update_offset(void __user *arg)
{
	struct fas_libgui_offset req;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	req.path[FAS_MAX_PATH_LEN - 1] = '\0';

	return fas_jank_set_libgui_offset(req.offset, req.path);
}

static long fas_ioctl_get_offset(void __user *arg)
{
	struct fas_libgui_offset resp;

	fas_jank_get_libgui_offset(&resp);

	if (copy_to_user(arg, &resp, sizeof(resp)))
		return -EFAULT;

	return 0;
}

static long fas_ioctl_register_listener(void __user *arg)
{
	struct fas_register_args req;
	long ret;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	ret = fas_jank_register_listener(req.pid, &req.ctx_id);
	if (ret)
		return ret;

	if (copy_to_user(arg, &req, sizeof(req))) {
		fas_jank_remove_listener(req.ctx_id);
		return -EFAULT;
	}

	return 0;
}

static long fas_ioctl_remove_listener(void __user *arg)
{
	struct fas_remove_args req;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	return fas_jank_remove_listener(req.ctx_id);
}

static long fas_ioctl_hint_frametime(void __user *arg)
{
	struct fas_frametime_hint req;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	return fas_jank_hint_frametime(req.ctx_id, req.fps);
}

static long fas_ioctl_set_target_fps_list(void __user *arg)
{
	struct fas_target_fps_list req;

	if (copy_from_user(&req, arg, sizeof(req)))
		return -EFAULT;

	return fas_jank_set_target_list(req.ctx_id, &req);
}

long fas_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;

	if (!fas_caller_is_root())
		return -EPERM;

	if (_IOC_TYPE(cmd) != FAS_IOC_MAGIC || _IOC_NR(cmd) > FAS_IOC_MAXNR)
		return -ENOTTY;

	switch (cmd) {
	case FAS_IOC_GET_VERSION:
		return fas_ioctl_get_version(uarg);
	case FAS_IOC_UPDATE_LIBGUI_OFFSET:
		return fas_ioctl_update_offset(uarg);
	case FAS_IOC_GET_LIBGUI_OFFSET:
		return fas_ioctl_get_offset(uarg);
	case FAS_IOC_REGISTER_LISTENER:
		return fas_ioctl_register_listener(uarg);
	case FAS_IOC_REMOVE_LISTENER:
		return fas_ioctl_remove_listener(uarg);
	case FAS_IOC_HINT_FRAMETIME:
		return fas_ioctl_hint_frametime(uarg);
	case FAS_IOC_SET_TARGET_FPS_LIST:
		return fas_ioctl_set_target_fps_list(uarg);
	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
long fas_ioctl_compat(struct file *file, unsigned int cmd, unsigned long arg)
{
	return fas_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif
