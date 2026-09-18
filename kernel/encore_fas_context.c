#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/err.h>

#include "encore_fas_context.h"

static DEFINE_IDR(fas_ctx_idr);
static DEFINE_MUTEX(fas_ctx_lock);
static LIST_HEAD(fas_ctx_list);

int fas_registry_init(void)
{
	return 0;
}

void fas_registry_exit(void)
{
	idr_destroy(&fas_ctx_idr);
}

static void fas_ctx_release(struct kref *kref)
{
	struct fas_context *ctx = container_of(kref, struct fas_context, kref);

	put_pid(ctx->pid_struct);
	kfree(ctx);
}

struct fas_context *fas_ctx_alloc(void)
{
	struct fas_context *ctx;
	int id;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	kref_init(&ctx->kref);
	atomic_set(&ctx->active, 1);
	INIT_LIST_HEAD(&ctx->node);

	mutex_lock(&fas_ctx_lock);
	id = idr_alloc(&fas_ctx_idr, ctx, 1, 0, GFP_KERNEL);
	mutex_unlock(&fas_ctx_lock);

	if (id < 0) {
		kfree(ctx);
		return ERR_PTR(id);
	}
	ctx->id = id;

	return ctx;
}

void fas_ctx_publish(struct fas_context *ctx)
{
	mutex_lock(&fas_ctx_lock);
	list_add_tail(&ctx->node, &fas_ctx_list);
	mutex_unlock(&fas_ctx_lock);
}

void fas_ctx_registry_remove(struct fas_context *ctx)
{
	mutex_lock(&fas_ctx_lock);
	idr_remove(&fas_ctx_idr, ctx->id);
	if (!list_empty(&ctx->node))
		list_del_init(&ctx->node);
	mutex_unlock(&fas_ctx_lock);
}

struct fas_context *fas_ctx_lookup_get(int ctx_id)
{
	struct fas_context *ctx;

	mutex_lock(&fas_ctx_lock);
	ctx = idr_find(&fas_ctx_idr, ctx_id);
	if (ctx && !kref_get_unless_zero(&ctx->kref))
		ctx = NULL;
	mutex_unlock(&fas_ctx_lock);

	return ctx;
}

void fas_ctx_put(struct fas_context *ctx)
{
	kref_put(&ctx->kref, fas_ctx_release);
}

void fas_ctx_for_each_matching_pid(struct task_struct *task,
				   void (*fn)(struct fas_context *ctx))
{
	struct fas_context *ctx;

	mutex_lock(&fas_ctx_lock);
	list_for_each_entry(ctx, &fas_ctx_list, node) {
		if (atomic_read(&ctx->active) &&
		    pid_task(ctx->pid_struct, PIDTYPE_PID) == task) {
			fn(ctx);
			break;
		}
	}
	mutex_unlock(&fas_ctx_lock);
}

void fas_ctx_teardown_all(void (*teardown_fn)(struct fas_context *ctx))
{
	struct fas_context *ctx, *tmp;

	mutex_lock(&fas_ctx_lock);
	list_for_each_entry_safe(ctx, tmp, &fas_ctx_list, node) {
		mutex_unlock(&fas_ctx_lock);
		teardown_fn(ctx);
		mutex_lock(&fas_ctx_lock);
	}
	mutex_unlock(&fas_ctx_lock);
}
