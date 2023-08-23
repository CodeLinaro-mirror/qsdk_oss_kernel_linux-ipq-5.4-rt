/*
 * Copyright (C) 2005-2007 Red Hat GmbH
 *
 * A target that delays reads and/or writes and can send
 * them to different devices.
 *
 * This file is released under the GPL.
 */

/*
Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
*/

#include <linux/err.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <crypto/ice.h>
#include <linux/qcom_scm.h>
#include <linux/device-mapper.h>

#define DM_MSG_PREFIX "inline-crypt"

#define MAX_MSM_ICE_KEY_LUT_SIZE 32
#define DM_REQ_CRYPT_ERROR -1

static struct ice_crypto_setting *ice_settings;

struct inlinecrypt_class {
	struct dm_dev *dev;
	sector_t start;
	unsigned inlinecrypt;
	unsigned ops;
};

struct inlinecrypt_c {
	struct timer_list inlinecrypt_timer;
	struct mutex timer_lock;
	struct workqueue_struct *kinlinecryptd_wq;
	struct work_struct flush_expired_bios;
	struct list_head inlinecrypted_bios;
	atomic_t may_inlinecrypt;

	struct inlinecrypt_class read;
	struct inlinecrypt_class write;
	struct inlinecrypt_class flush;

	int argc;
};

struct dm_inlinecrypt_info {
	struct inlinecrypt_c *context;
	struct inlinecrypt_class *class;
	struct list_head list;
	unsigned long expires;
};

static DEFINE_MUTEX(inlinecrypted_bios_lock);

static void handle_inlinecrypted_timer(struct timer_list *t)
{
	struct inlinecrypt_c *dc = from_timer(dc, t, inlinecrypt_timer);

	queue_work(dc->kinlinecryptd_wq, &dc->flush_expired_bios);
}

static void queue_timeout(struct inlinecrypt_c *dc, unsigned long expires)
{
	mutex_lock(&dc->timer_lock);

	if (!timer_pending(&dc->inlinecrypt_timer) || expires < dc->inlinecrypt_timer.expires)
		mod_timer(&dc->inlinecrypt_timer, expires);

	mutex_unlock(&dc->timer_lock);
}

static void flush_bios(struct bio *bio)
{
	struct bio *n;

	while (bio) {
		n = bio->bi_next;
		bio->bi_next = NULL;
		generic_make_request(bio);
		bio = n;
	}
}

static struct bio *flush_inlinecrypted_bios(struct inlinecrypt_c *dc, int flush_all)
{
	struct dm_inlinecrypt_info *inlinecrypted, *next;
	unsigned long next_expires = 0;
	unsigned long start_timer = 0;
	struct bio_list flush_bios = { };

	mutex_lock(&inlinecrypted_bios_lock);
	list_for_each_entry_safe(inlinecrypted, next, &dc->inlinecrypted_bios, list) {
		if (flush_all || time_after_eq(jiffies, inlinecrypted->expires)) {
			struct bio *bio = dm_bio_from_per_bio_data(inlinecrypted,
						sizeof(struct dm_inlinecrypt_info));
			list_del(&inlinecrypted->list);
			bio_list_add(&flush_bios, bio);
			inlinecrypted->class->ops--;
			continue;
		}

		if (!start_timer) {
			start_timer = 1;
			next_expires = inlinecrypted->expires;
		} else
			next_expires = min(next_expires, inlinecrypted->expires);
	}
	mutex_unlock(&inlinecrypted_bios_lock);

	if (start_timer)
		queue_timeout(dc, next_expires);

	return bio_list_get(&flush_bios);
}

static void flush_expired_bios(struct work_struct *work)
{
	struct inlinecrypt_c *dc;

	dc = container_of(work, struct inlinecrypt_c, flush_expired_bios);
	flush_bios(flush_inlinecrypted_bios(dc, 0));
}

static void inlinecrypt_dtr(struct dm_target *ti)
{
	struct inlinecrypt_c *dc = ti->private;

	if (dc->kinlinecryptd_wq)
		destroy_workqueue(dc->kinlinecryptd_wq);

	if (dc->read.dev)
		dm_put_device(ti, dc->read.dev);
	if (dc->write.dev)
		dm_put_device(ti, dc->write.dev);
	if (dc->flush.dev)
		dm_put_device(ti, dc->flush.dev);

	kfree(ice_settings);
	ice_settings = NULL;

	mutex_destroy(&dc->timer_lock);

	kfree(dc);
}

static int inlinecrypt_class_ctr(struct dm_target *ti, struct inlinecrypt_class *c, char **argv)
{
	int ret;
	unsigned long long tmpll;
	char dummy;

	if (sscanf(argv[5], "%llu%c", &tmpll, &dummy) != 1 || tmpll != (sector_t)tmpll) {
		ti->error = "Invalid device sector";
		return -EINVAL;
	}
	c->start = tmpll;

	if (sscanf(argv[5], "%u%c", &c->inlinecrypt, &dummy) != 1) {
		ti->error = "Invalid inlinecrypt";
		return -EINVAL;
	}

	ret = dm_get_device(ti, argv[3], dm_table_get_mode(ti->table), &c->dev);
	if (ret) {
		ti->error = "Device lookup failed";
		return ret;
	}

	return 0;
}

static int qcom_set_ice_context(char **argv)
{
	uint8_t *hex_data_context = NULL, *hex_salt_context = NULL;
	uint32_t hex_salt_len = 0, hex_data_len = 0;
	uint32_t seedtype = 0;
	char *buf = NULL;
	int i, ret;

	hex_data_context  = kmalloc(DATA_COTEXT_LEN, GFP_KERNEL);
	if (!hex_data_context) {
		DMERR("%s: no memory allocated\n", __func__);
		return -ENOMEM;
	}

	if (!strcmp(argv[2], "oemseed")) {
		seedtype = 1;
		buf = argv[5];
		hex_data_len = strlen(argv[5]) / 2;
		for (i = 0; i < hex_data_len; i++) {
			sscanf(buf, "%2hhx", &hex_data_context[i]);
			buf += 2;
		}
	}

	if (ice_settings->algo_mode == ICE_CRYPTO_ALGO_MODE_AES_XTS &&
			seedtype == 1) {
		hex_salt_context = kmalloc(SALT_COTEXT_LEN, GFP_KERNEL);
		if (!hex_salt_context) {
			DMERR("%s: no memory allocated\n", __func__);
			return -ENOMEM;
		}

		buf = argv[6];
		hex_salt_len = strlen(argv[6]) / 2;
		for (i = 0; i < hex_salt_len; i++) {
			sscanf(buf, "%2hhx", &hex_salt_context[i]);
			buf += 2;
		}
		buf = NULL;
	}

	ret = qcom_context_sec_ice(seedtype, ice_settings->key_size,
			ice_settings->algo_mode, hex_data_context, hex_data_len,
			hex_salt_context, hex_salt_len);
	if (ret)
		DMERR("%s: ice context configuration fail\n", __func__);

	return ret;
}

static int qcom_set_ice_config(char **argv)
{
	struct ice_config_sec *ice;
	int ret;

	ice = kmalloc(sizeof(struct ice_config_sec), GFP_KERNEL);
	if (!ice) {
		DMERR("%s: no memory allocated\n", __func__);
		return -ENOMEM;
	}

	if (!strcmp(argv[2], "oemseed")) {
		ret = qcom_set_ice_context(argv);
		if (ret){
			DMERR("%s: ice configuration fail\n", __func__);
			return ret;
		}
	}

	/* update the ice config structure to send tz */
	ice->index = ice_settings->key_index;
	ice->key_size = ice_settings->key_size;
	ice->algo_mode = ice_settings->algo_mode;
	ice->key_mode = ice_settings->key_mode;

	ret = qcom_config_sec_ice(ice, sizeof(struct ice_config_sec));

	if (ret)
		DMERR("%s: ice configuration fail\n", __func__);

	kfree(ice);
	return ret;
}

/*
 * Mapping parameters:
 *    <device> <offset> <inlinecrypt> [<write_device> <write_offset> <write_inlinecrypt>]
 *
 * With separate write parameters, the first set is only used for reads.
 * Offsets are specified in sectors.
 * Delays are specified in milliseconds.
 */
static int inlinecrypt_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct inlinecrypt_c *dc;
	int ret;

	if (argc != 7) {
		ti->error = "Requires exactly 6 arguments";
		return -EINVAL;
	}

	dc = kzalloc(sizeof(*dc), GFP_KERNEL);
	if (!dc) {
		ti->error = "Cannot allocate context";
		return -ENOMEM;
	}

	ti->private = dc;
	timer_setup(&dc->inlinecrypt_timer, handle_inlinecrypted_timer, 0);
	INIT_WORK(&dc->flush_expired_bios, flush_expired_bios);
	INIT_LIST_HEAD(&dc->inlinecrypted_bios);
	mutex_init(&dc->timer_lock);
	atomic_set(&dc->may_inlinecrypt, 1);
	dc->argc = argc;

	ret = inlinecrypt_class_ctr(ti, &dc->read, argv);
	if (ret)
		goto bad;

	ret = inlinecrypt_class_ctr(ti, &dc->write, argv);
	if (ret)
		goto bad;

	ret = inlinecrypt_class_ctr(ti, &dc->flush, argv);
	if (ret)
		goto bad;

	dc->kinlinecryptd_wq = alloc_workqueue("kinlinecryptd", WQ_MEM_RECLAIM, 0);
	if (!dc->kinlinecryptd_wq) {
		ret = -EINVAL;
		DMERR("Couldn't start kinlinecryptd");
		goto bad;
	}

	/* configure ICE settings */
	ice_settings =
		kzalloc(sizeof(struct ice_crypto_setting), GFP_KERNEL);
	if (!ice_settings) {
		ret = -ENOMEM;
		goto bad;
	}

	if (!strcmp(argv[0], "aes-xts-128-hwkey0")) {
		ice_settings->key_size = ICE_CRYPTO_KEY_SIZE_128;
		ice_settings->algo_mode = ICE_CRYPTO_ALGO_MODE_AES_XTS;
		ice_settings->key_mode = ICE_CRYPTO_USE_KEY0_HW_KEY;
	} else if (!strcmp(argv[0], "aes-xts-128-hwkey1")) {
		ice_settings->key_size = ICE_CRYPTO_KEY_SIZE_128;
		ice_settings->algo_mode = ICE_CRYPTO_ALGO_MODE_AES_XTS;
		ice_settings->key_mode = ICE_CRYPTO_USE_KEY1_HW_KEY;
	} else if (!strcmp(argv[0], "aes-xts-256-hwkey0")) {
		ice_settings->key_size = ICE_CRYPTO_KEY_SIZE_256;
		ice_settings->algo_mode = ICE_CRYPTO_ALGO_MODE_AES_XTS;
		ice_settings->key_mode = ICE_CRYPTO_USE_KEY0_HW_KEY;
	} else if (!strcmp(argv[0], "aes-xts-256-hwkey1")) {
		ice_settings->key_size = ICE_CRYPTO_KEY_SIZE_256;
		ice_settings->algo_mode = ICE_CRYPTO_ALGO_MODE_AES_XTS;
		ice_settings->key_mode = ICE_CRYPTO_USE_KEY1_HW_KEY;
	} else if (!strcmp(argv[0], "aes-ecb-128-hwkey0")) {
		ice_settings->key_size = ICE_CRYPTO_KEY_SIZE_128;
		ice_settings->algo_mode = ICE_CRYPTO_ALGO_MODE_AES_ECB;
		ice_settings->key_mode = ICE_CRYPTO_USE_KEY0_HW_KEY;
	} else if (!strcmp(argv[0], "aes-ecb-128-hwkey1")) {
		ice_settings->key_size = ICE_CRYPTO_KEY_SIZE_128;
		ice_settings->algo_mode = ICE_CRYPTO_ALGO_MODE_AES_ECB;
		ice_settings->key_mode = ICE_CRYPTO_USE_KEY1_HW_KEY;
	} else if (!strcmp(argv[0], "aes-ecb-256-hwkey0")) {
		ice_settings->key_size = ICE_CRYPTO_KEY_SIZE_256;
		ice_settings->algo_mode = ICE_CRYPTO_ALGO_MODE_AES_ECB;
		ice_settings->key_mode = ICE_CRYPTO_USE_KEY0_HW_KEY;
	} else if (!strcmp(argv[0], "aes-ecb-256-hwkey1")) {
		ice_settings->key_size = ICE_CRYPTO_KEY_SIZE_256;
		ice_settings->algo_mode = ICE_CRYPTO_ALGO_MODE_AES_ECB;
		ice_settings->key_mode = ICE_CRYPTO_USE_KEY1_HW_KEY;
	} else {
		ret = DM_REQ_CRYPT_ERROR;
		goto bad;
	}

	if (kstrtou16(argv[1], 0, &ice_settings->key_index) ||
		ice_settings->key_index < 0 ||
		ice_settings->key_index > MAX_MSM_ICE_KEY_LUT_SIZE) {
		DMERR("%s Err: key index %d received for ICE\n",
				__func__, ice_settings->key_index);
		ret = DM_REQ_CRYPT_ERROR;
		goto bad;
	}

	ti->num_flush_bios = 1;
	ti->num_discard_bios = 1;
	ti->per_io_data_size = sizeof(struct dm_inlinecrypt_info);

	ret = qcom_set_ice_config(argv);
	if (ret) {
		DMERR("%s: ice configuration fail\n", __func__);
		goto bad;
	}

	DMINFO("%s: Mapping block_device %s to dm-inline-crypt ok!\n",
		__func__, argv[3]);

	return 0;

bad:
	inlinecrypt_dtr(ti);
	return ret;
}

static int inlinecrypt_bio(struct inlinecrypt_c *dc, struct inlinecrypt_class *c, struct bio *bio)
{
	struct dm_inlinecrypt_info *inlinecrypted;
	unsigned long expires = 0;

	if (!c->inlinecrypt || !atomic_read(&dc->may_inlinecrypt))
		return DM_MAPIO_REMAPPED;

	inlinecrypted = dm_per_bio_data(bio, sizeof(struct dm_inlinecrypt_info));

	inlinecrypted->context = dc;
	inlinecrypted->expires = expires = jiffies + msecs_to_jiffies(c->inlinecrypt);

	mutex_lock(&inlinecrypted_bios_lock);
	c->ops++;
	list_add_tail(&inlinecrypted->list, &dc->inlinecrypted_bios);
	mutex_unlock(&inlinecrypted_bios_lock);

	queue_timeout(dc, expires);

	return DM_MAPIO_SUBMITTED;
}

static void inlinecrypt_presuspend(struct dm_target *ti)
{
	struct inlinecrypt_c *dc = ti->private;

	atomic_set(&dc->may_inlinecrypt, 0);
	del_timer_sync(&dc->inlinecrypt_timer);
	flush_bios(flush_inlinecrypted_bios(dc, 1));
}

static void inlinecrypt_resume(struct dm_target *ti)
{
	struct inlinecrypt_c *dc = ti->private;

	atomic_set(&dc->may_inlinecrypt, 1);
}

static int inlinecrypt_map(struct dm_target *ti, struct bio *bio)
{
	struct inlinecrypt_c *dc = ti->private;
	struct inlinecrypt_class *c;
	struct dm_inlinecrypt_info *inlinecrypted = dm_per_bio_data(bio, sizeof(struct dm_inlinecrypt_info));

	if (bio_data_dir(bio) == WRITE) {
		if (unlikely(bio->bi_opf & REQ_PREFLUSH))
			c = &dc->flush;
		else
			c = &dc->write;
	} else {
		c = &dc->read;
	}
	inlinecrypted->class = c;
	bio_set_dev(bio, c->dev->bdev);
	if (bio_sectors(bio))
		bio->bi_iter.bi_sector = c->start + dm_target_offset(ti, bio->bi_iter.bi_sector);

	return inlinecrypt_bio(dc, c, bio);
}

#define DMEMIT_DELAY_CLASS(c) \
	DMEMIT("%s %llu %u", (c)->dev->name, (unsigned long long)(c)->start, (c)->inlinecrypt)

static void inlinecrypt_status(struct dm_target *ti, status_type_t type,
			 unsigned status_flags, char *result, unsigned maxlen)
{
	struct inlinecrypt_c *dc = ti->private;
	int sz = 0;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%u %u %u", dc->read.ops, dc->write.ops, dc->flush.ops);
		break;

	case STATUSTYPE_TABLE:
		DMEMIT_DELAY_CLASS(&dc->read);
		if (dc->argc >= 6) {
			DMEMIT(" ");
			DMEMIT_DELAY_CLASS(&dc->write);
		}
		if (dc->argc >= 9) {
			DMEMIT(" ");
			DMEMIT_DELAY_CLASS(&dc->flush);
		}
		break;
	}
}

static int inlinecrypt_iterate_devices(struct dm_target *ti,
				 iterate_devices_callout_fn fn, void *data)
{
	struct inlinecrypt_c *dc = ti->private;
	int ret = 0;

	ret = fn(ti, dc->read.dev, dc->read.start, ti->len, data);
	if (ret)
		goto out;
	ret = fn(ti, dc->write.dev, dc->write.start, ti->len, data);
	if (ret)
		goto out;
	ret = fn(ti, dc->flush.dev, dc->flush.start, ti->len, data);
	if (ret)
		goto out;

out:
	return ret;
}

static struct target_type inlinecrypt_target = {
	.name	     = "inline-crypt",
	.version     = {1, 2, 1},
	.features    = DM_TARGET_PASSES_INTEGRITY,
	.module      = THIS_MODULE,
	.ctr	     = inlinecrypt_ctr,
	.dtr	     = inlinecrypt_dtr,
	.map	     = inlinecrypt_map,
	.presuspend  = inlinecrypt_presuspend,
	.resume	     = inlinecrypt_resume,
	.status	     = inlinecrypt_status,
	.iterate_devices = inlinecrypt_iterate_devices,
};

static int __init dm_inlinecrypt_init(void)
{
	int r;

	r = dm_register_target(&inlinecrypt_target);
	if (r < 0) {
		DMERR("register failed %d", r);
		goto bad_register;
	}

	return 0;

bad_register:
	return r;
}

static void __exit dm_inlinecrypt_exit(void)
{
	dm_unregister_target(&inlinecrypt_target);
}

/* Module hooks */
module_init(dm_inlinecrypt_init);
module_exit(dm_inlinecrypt_exit);

MODULE_DESCRIPTION(DM_NAME " inline crypt");
MODULE_LICENSE("GPL");
