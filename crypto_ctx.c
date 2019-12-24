// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2019 Samsung Electronics Co., Ltd.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/version.h>

#include "crypto_ctx.h"
#include "buffer_pool.h"

struct crypto_ctx_list {
	spinlock_t		ctx_lock;
	int			avail_ctx;
	struct list_head	idle_ctx;
	wait_queue_head_t	ctx_wait;
};

static struct crypto_ctx_list ctx_list;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
static inline void free_blk(struct blkcipher_desc *desc)
{
	if (desc) {
		crypto_free_blkcipher(desc->tfm);
		kfree(desc);
	}
}
#endif

static inline void free_aead(struct crypto_aead *aead)
{
	if (aead)
		crypto_free_aead(aead);
}

static void free_shash(struct shash_desc *shash)
{
	if (shash) {
		crypto_free_shash(shash->tfm);
		kfree(shash);
	}
}

static struct crypto_aead *alloc_aead(int id)
{
	struct crypto_aead *tfm = NULL;

	switch (id) {
	case CRYPTO_AEAD_AES128_GCM:
		tfm = crypto_alloc_aead("gcm(aes)", 0, 0);
		break;
	case CRYPTO_AEAD_AES128_CCM:
		tfm = crypto_alloc_aead("ccm(aes)", 0, 0);
		break;
	}

	if (IS_ERR(tfm))
		return NULL;

	return tfm;
}

static struct shash_desc *alloc_shash_desc(int id)
{
	struct crypto_shash *tfm = NULL;
	struct shash_desc *shash;

	switch (id) {
	case CRYPTO_SHASH_HMACMD5:
		tfm = crypto_alloc_shash("hmac(md5)", 0, 0);
		break;
	case CRYPTO_SHASH_HMACSHA256:
		tfm = crypto_alloc_shash("hmac(sha256)", 0, 0);
		break;
	case CRYPTO_SHASH_CMACAES:
		tfm = crypto_alloc_shash("cmac(aes)", 0, 0);
		break;
	case CRYPTO_SHASH_SHA512:
		tfm = crypto_alloc_shash("sha512", 0, 0);
		break;
	case CRYPTO_SHASH_MD4:
		tfm = crypto_alloc_shash("md4", 0, 0);
		break;
	case CRYPTO_SHASH_MD5:
		tfm = crypto_alloc_shash("md5", 0, 0);
		break;
	}

	if (IS_ERR(tfm))
		return NULL;

	shash = kzalloc(sizeof(*shash) + crypto_shash_descsize(tfm),
			GFP_KERNEL);
	if (!shash)
		crypto_free_shash(tfm);
	else
		shash->tfm = tfm;
	return shash;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
static struct blkcipher_desc *alloc_blk_desc(int id)
{
	struct crypto_blkcipher *tfm = NULL;
	struct blkcipher_desc *desc;

	switch (id) {
	case CRYPTO_BLK_ECBDES:
		tfm = crypto_alloc_blkcipher("ecb(des)", 0, CRYPTO_ALG_ASYNC);
		break;
	}

	if (IS_ERR(tfm))
		return NULL;

	desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc)
		crypto_free_blkcipher(tfm);
	else
		desc->tfm = tfm;
	return desc;
}
#endif

static struct cifsd_crypto_ctx *ctx_alloc(void)
{
	return cifsd_alloc(sizeof(struct cifsd_crypto_ctx));
}

static void ctx_free(struct cifsd_crypto_ctx *ctx)
{
	int i;

	for (i = 0; i < CRYPTO_SHASH_MAX; i++)
		free_shash(ctx->desc[i]);
	for (i = 0; i < CRYPTO_AEAD_MAX; i++)
		free_aead(ctx->ccmaes[i]);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
	for (i = 0; i < CRYPTO_BLK_MAX; i++)
		free_blk(ctx->blk_desc[i]);
#endif
	cifsd_free(ctx);
}

static struct cifsd_crypto_ctx *cifsd_find_crypto_ctx(void)
{
	struct cifsd_crypto_ctx *ctx;

	while (1) {
		spin_lock(&ctx_list.ctx_lock);
		if (!list_empty(&ctx_list.idle_ctx)) {
			ctx = list_entry(ctx_list.idle_ctx.next,
					  struct cifsd_crypto_ctx,
					  list);
			list_del(&ctx->list);
			spin_unlock(&ctx_list.ctx_lock);
			return ctx;
		}

		if (ctx_list.avail_ctx > num_online_cpus()) {
			spin_unlock(&ctx_list.ctx_lock);
			wait_event(ctx_list.ctx_wait,
				   !list_empty(&ctx_list.idle_ctx));
			continue;
		}

		ctx_list.avail_ctx++;
		spin_unlock(&ctx_list.ctx_lock);

		ctx = ctx_alloc();
		if (!ctx) {
			spin_lock(&ctx_list.ctx_lock);
			ctx_list.avail_ctx--;
			spin_unlock(&ctx_list.ctx_lock);
			wait_event(ctx_list.ctx_wait,
				   !list_empty(&ctx_list.idle_ctx));
			continue;
		}
		break;
	}
	return ctx;
}

void cifsd_release_crypto_ctx(struct cifsd_crypto_ctx *ctx)
{
	if (!ctx)
		return;

	spin_lock(&ctx_list.ctx_lock);
	if (ctx_list.avail_ctx <= num_online_cpus()) {
		list_add(&ctx->list, &ctx_list.idle_ctx);
		spin_unlock(&ctx_list.ctx_lock);
		wake_up(&ctx_list.ctx_wait);
		return;
	}

	ctx_list.avail_ctx--;
	spin_unlock(&ctx_list.ctx_lock);
	ctx_free(ctx);
}

static struct cifsd_crypto_ctx *____crypto_shash_ctx_find(int id)
{
	struct cifsd_crypto_ctx *ctx;

	if (id >= CRYPTO_SHASH_MAX)
		return NULL;

	ctx = cifsd_find_crypto_ctx();
	if (ctx->desc[id])
		return ctx;

	ctx->desc[id] = alloc_shash_desc(id);
	if (ctx->desc[id])
		return ctx;
	cifsd_release_crypto_ctx(ctx);
	return NULL;
}

struct cifsd_crypto_ctx *cifsd_crypto_ctx_find_hmacmd5(void)
{
	return ____crypto_shash_ctx_find(CRYPTO_SHASH_HMACMD5);
}

struct cifsd_crypto_ctx *cifsd_crypto_ctx_find_hmacsha256(void)
{
	return ____crypto_shash_ctx_find(CRYPTO_SHASH_HMACSHA256);
}

struct cifsd_crypto_ctx *cifsd_crypto_ctx_find_cmacaes(void)
{
	return ____crypto_shash_ctx_find(CRYPTO_SHASH_CMACAES);
}

struct cifsd_crypto_ctx *cifsd_crypto_ctx_find_sha512(void)
{
	return ____crypto_shash_ctx_find(CRYPTO_SHASH_SHA512);
}

struct cifsd_crypto_ctx *cifsd_crypto_ctx_find_md4(void)
{
	return ____crypto_shash_ctx_find(CRYPTO_SHASH_MD4);
}

struct cifsd_crypto_ctx *cifsd_crypto_ctx_find_md5(void)
{
	return ____crypto_shash_ctx_find(CRYPTO_SHASH_MD5);
}

static struct cifsd_crypto_ctx *____crypto_aead_ctx_find(int id)
{
	struct cifsd_crypto_ctx *ctx;

	if (id >= CRYPTO_AEAD_MAX)
		return NULL;

	ctx = cifsd_find_crypto_ctx();
	if (ctx->ccmaes[id])
		return ctx;

	ctx->ccmaes[id] = alloc_aead(id);
	if (ctx->ccmaes[id])
		return ctx;
	cifsd_release_crypto_ctx(ctx);
	return NULL;
}

struct cifsd_crypto_ctx *cifsd_crypto_ctx_find_gcm(void)
{
	return ____crypto_aead_ctx_find(CRYPTO_AEAD_AES128_GCM);
}

struct cifsd_crypto_ctx *cifsd_crypto_ctx_find_ccm(void)
{
	return ____crypto_aead_ctx_find(CRYPTO_AEAD_AES128_CCM);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
static struct cifsd_crypto_ctx *____crypto_blk_ctx_find(int id)
{
	struct cifsd_crypto_ctx *ctx;

	if (id >= CRYPTO_BLK_MAX)
		return NULL;

	ctx = cifsd_find_crypto_ctx();
	if (ctx->blk_desc[id])
		return ctx;

	ctx->blk_desc[id] = alloc_blk_desc(id);
	if (ctx->blk_desc[id])
		return ctx;
	cifsd_release_crypto_ctx(ctx);
	return NULL;
}

struct cifsd_crypto_ctx *cifsd_crypto_ctx_find_ecbdes(void)
{
	return ____crypto_blk_ctx_find(CRYPTO_BLK_ECBDES);
}
#endif

void cifsd_crypto_destroy(void)
{
	struct cifsd_crypto_ctx *ctx;

	while (!list_empty(&ctx_list.idle_ctx)) {
		ctx = list_entry(ctx_list.idle_ctx.next,
				 struct cifsd_crypto_ctx,
				 list);
		list_del(&ctx->list);
		ctx_free(ctx);
	}
}

int cifsd_crypto_create(void)
{
	struct cifsd_crypto_ctx *ctx;

	spin_lock_init(&ctx_list.ctx_lock);
	INIT_LIST_HEAD(&ctx_list.idle_ctx);
	init_waitqueue_head(&ctx_list.ctx_wait);
	ctx_list.avail_ctx = 1;

	ctx = ctx_alloc();
	if (!ctx)
		return -ENOMEM;
	list_add(&ctx->list, &ctx_list.idle_ctx);
	return 0;
}