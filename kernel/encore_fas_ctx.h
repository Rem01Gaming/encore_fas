// SPDX-License-Identifier: GPL-2.0-only

#ifndef ENCORE_FAS_CTX_H
#define ENCORE_FAS_CTX_H

#include "uapi/encore_fas_uapi.h"

void fas_ctx_init(void);
void fas_ctx_exit(void);

int fas_ctx_register(struct fas_register_args *req);
int fas_ctx_remove(s32 ctx_id);
int fas_ctx_set_config(s32 ctx_id, const struct fas_config *cfg);
int fas_ctx_get_state(struct fas_state *state);
void fas_ctx_list(struct fas_listener_list *list);

#endif
