// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/err.h>
#include <linux/key.h>
#include <linux/moduleparam.h>
#include <linux/qcom_scm.h>

#define CONTEXT_SIZE		128
#define MAX_KEY_SIZE		32

#define KEYRING_TYPE		"user"
#define KEYRING_DESC_HW_KEY_O	"hw_key_o"
#define KEYRING_DESC_HW_KEY_CR	"hw_key_cr"

static char context[CONTEXT_SIZE];

extern int look_up_user_keyrings(struct key **, struct key **);

__init int hw_key_gen_store(char *context, size_t context_len,
			    const char *type, const char *description,
			    uint8_t *hw_key, u32 hw_key_len)
{
	key_ref_t key_ref, keyring_ref;
	struct key *user_key_ref;
	int ret;

	if (hw_key == NULL)
		return -EINVAL;

	if (hw_key_len != 16 && hw_key_len != 32)
		return -EINVAL;

	ret = qti_scm_derive_and_share_key(QTI_SCM_DERIVE_KEY,
			QTI_SCM_DERIVE_KEY_PARAM_ID, hw_key_len,
			context, (u32)context_len, hw_key, hw_key_len);
	if (ret < 0)
		goto error;

	ret = look_up_user_keyrings(&user_key_ref, NULL);
	if (ret < 0)
		goto error;

	keyring_ref = make_key_ref(user_key_ref, 1);
	if (IS_ERR(keyring_ref)) {
		ret = PTR_ERR(keyring_ref);
		return ret;
	}

	key_ref = key_create_or_update(keyring_ref, type, description,
				       hw_key, hw_key_len, KEY_PERM_UNDEF,
				       KEY_ALLOC_IN_QUOTA);
	if (!IS_ERR(key_ref)) {
		key_ref_put(key_ref);
		ret = 0;
	} else {
		ret = PTR_ERR(key_ref);
	}

	key_ref_put(keyring_ref);
error:
	return ret;
}

static __init int tmel_hw_key_init(void)
{
	size_t context_len = strlen(context);
	uint8_t hw_key_o[MAX_KEY_SIZE] = {0}, hw_key_cr[MAX_KEY_SIZE] = {0};
	u32 hw_key_o_len = MAX_KEY_SIZE, hw_key_cr_len = MAX_KEY_SIZE;
	int ret = 0;

	if (context_len == 0) {
		pr_err("Context is not provided, skipping key init");
		return ret;
	}

	ret = hw_key_gen_store(context, context_len, KEYRING_TYPE,
			       KEYRING_DESC_HW_KEY_O, hw_key_o, hw_key_o_len);
	if (ret) {
		pr_err("Failed to generate key for given context, error = 0x%x",
		       ret);
		return ret;
	}

	ret = hw_key_gen_store(NULL, 0, KEYRING_TYPE, KEYRING_DESC_HW_KEY_CR,
			       hw_key_cr, hw_key_cr_len);
	if (ret) {
		pr_err("Failed to generate key for NULL context, error = 0x%x",
		       ret);
		return ret;
	}

	return ret;
}

module_param_string(context, context, sizeof(context), 0);
device_initcall(tmel_hw_key_init);
