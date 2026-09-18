// SPDX-License-Identifier: GPL-2.0-only

#include "encore_fas_det.h"

u64 hotpath_frame(struct fas_hot *h, struct fas_cfg *c, u64 now,
		  struct fas_out *out)
{
	return fas_det_frame(h, c, now, out);
}

u64 hotpath_watchdog(struct fas_hot *h, struct fas_cfg *c, u64 now,
		     struct fas_out *out)
{
	return fas_det_wd_fire(h, c, now, out);
}
