/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include "soc/qcom/license_manager.h"

struct qmi_handle *lm_clnt_hdl;

static struct lm_svc_ctx *lm_svc;

static struct kobject *lm_kobj;

static unsigned int use_license_from_rootfs = 0;

module_param(use_license_from_rootfs, uint, 0644);
MODULE_PARM_DESC(use_license_from_rootfs, "Use license files from rootfs: 0,1");

static const char * const license_path = "/license";
static const char * const license_extn = ".pfm";

struct qmi_elem_info qmi_lm_feature_list_req_msg_v01_ei[] = {
	{
		.data_type      = QMI_UNSIGNED_4_BYTE,
		.elem_len       = 1,
		.elem_size      = sizeof(u32),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x01,
		.offset         = offsetof(struct
					   qmi_lm_feature_list_req_msg_v01,
					   reserved),
	},
	{
		.data_type      = QMI_OPT_FLAG,
		.elem_len       = 1,
		.elem_size      = sizeof(u8),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x10,
		.offset         = offsetof(struct
					   qmi_lm_feature_list_req_msg_v01,
					   feature_list_valid),
	},
	{
		.data_type      = QMI_DATA_LEN,
		.elem_len       = 1,
		.elem_size      = sizeof(u8),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x10,
		.offset         = offsetof(struct
					   qmi_lm_feature_list_req_msg_v01,
					   feature_list_len),
	},
	{
		.data_type      = QMI_UNSIGNED_4_BYTE,
		.elem_len       = QMI_LM_MAX_FEATURE_LIST_V01,
		.elem_size      = sizeof(u32),
		.array_type       = VAR_LEN_ARRAY,
		.tlv_type       = 0x10,
		.offset         = offsetof(struct
					   qmi_lm_feature_list_req_msg_v01,
					   feature_list),
	},
	{
		.data_type      = QMI_EOTI,
		.array_type       = NO_ARRAY,
		.tlv_type       = QMI_COMMON_TLV_TYPE,
	},
};
EXPORT_SYMBOL(qmi_lm_feature_list_req_msg_v01_ei);

struct qmi_elem_info qmi_lm_feature_list_resp_msg_v01_ei[] = {
	{
		.data_type      = QMI_STRUCT,
		.elem_len       = 1,
		.elem_size      = sizeof(struct qmi_response_type_v01),
		.array_type       = NO_ARRAY,
		.tlv_type       = 0x02,
		.offset         = offsetof(struct
					   qmi_lm_feature_list_resp_msg_v01,
					   resp),
		.ei_array      = qmi_response_type_v01_ei,
	},
	{
		.data_type      = QMI_EOTI,
		.array_type       = NO_ARRAY,
		.tlv_type       = QMI_COMMON_TLV_TYPE,
	},
};
EXPORT_SYMBOL(qmi_lm_feature_list_resp_msg_v01_ei);

static void qmi_handle_feature_list_req(struct qmi_handle *handle,
			struct sockaddr_qrtr *sq,
			struct qmi_txn *txn,
			const void *decoded_msg)
{
	struct qmi_lm_feature_list_req_msg_v01 *req;
	struct qmi_lm_feature_list_resp_msg_v01 *resp;
	struct feature_info *licensed_features, *itr, *tmp;
	int i, ret;

	req = (struct qmi_lm_feature_list_req_msg_v01 *)decoded_msg;

	pr_debug("Licensed Features: Request rcvd, node_id: 0x%x", sq->sq_node);

	resp = kzalloc(sizeof(*resp), GFP_KERNEL);
	if (!resp) {
		pr_err("%s: Memory allocation failed for resp buffer\n",
							__func__);
		goto free_resp_buf;
	}
	resp->resp.result = QMI_RESULT_FAILURE_V01;

	if(!req->feature_list_valid) {
		pr_err("No Features are licensed in node 0x%x\n",sq->sq_node);
	}

	licensed_features = kzalloc(sizeof(*licensed_features), GFP_KERNEL);
	if (!licensed_features) {
		pr_err("%s: Memory allocation failed for feature list\n",
							__func__);
		goto send_resp;
	}

	licensed_features->sq_node = sq->sq_node;
	licensed_features->sq_port = sq->sq_port;
	licensed_features->reserved = req->reserved;
	if(!req->feature_list_valid) {
		licensed_features->len = 0;
	} else {
		licensed_features->len = req->feature_list_len;
		if(licensed_features->len > QMI_LM_MAX_FEATURE_LIST_V01) {
			pr_err("Feature_list larger than QMI_MAX_FEATURE_LIST_V01"
					"%d, so assiging it to max \n",
					QMI_LM_MAX_FEATURE_LIST_V01);
			licensed_features->len = QMI_LM_MAX_FEATURE_LIST_V01;
		}

		for(i =0; i<licensed_features->len; i++)
			licensed_features->list[i] = req->feature_list[i];

	}

	if (!list_empty(&lm_svc->clients_feature_list)) {
		list_for_each_entry_safe(itr, tmp, &lm_svc->clients_feature_list,
								node) {
			if (itr->sq_node == sq->sq_node && itr->sq_port == sq->sq_port) {
				list_del(&itr->node);
				kfree(itr);
			}
		}
	}

	list_add_tail(&licensed_features->node, &lm_svc->clients_feature_list);
	resp->resp.result = QMI_RESULT_SUCCESS_V01;

send_resp:
	ret = qmi_send_response(handle, sq, txn,
			QMI_LM_FEATURE_LIST_RESP_V01,
			QMI_LM_FEATURE_LIST_RESP_MSG_V01_MAX_MSG_LEN,
			qmi_lm_feature_list_resp_msg_v01_ei, resp);
	if (ret < 0)
		pr_err("%s: Sending license termination response failed"
					"with error_code:%d\n",__func__,ret);
	else
		pr_debug("Licensed Features: Response sent, Result code "
			"%d\n", resp->resp.result);
free_resp_buf:
	kfree(resp);

}

static ssize_t show_licensed_features(struct kobject *k,
				struct kobj_attribute *attr, char *buf)
{
	uint32_t i, len = 0, max_buf_len = PAGE_SIZE;
	struct feature_info *itr, *tmp;

	if (!list_empty(&lm_svc->clients_feature_list)) {
		list_for_each_entry_safe(itr, tmp,
					&lm_svc->clients_feature_list, node) {
			if(itr->len == 0) {
				len += scnprintf(buf+len, max_buf_len-len,
					"\nClient Node:0x%x Port:%d,"
					" No feature licensed\n",itr->sq_node,
					itr->sq_port);
			} else {
				len += scnprintf(buf+len, max_buf_len-len,
					"\nClient Node:0x%x Port:%d,"
					" %d features licensed\n"
					" Feature List:\n", itr->sq_node,
					itr->sq_port, itr->len);
				for(i=0;i<itr->len;i++)
					 len += scnprintf(buf+len,
						max_buf_len-len,
						" %d\n",itr->list[i]);
			}
			len += scnprintf(buf+len, max_buf_len-len,
					"\nAdditional Info: 0x%08x\n",
					itr->reserved);
		}
	} else
		len += scnprintf(buf+len, max_buf_len-len,
			"Client's licensed feature list not available\n");

	return len;
}

static struct kobj_attribute lm_licensed_features_attr =
	__ATTR(licensed_features, 0400, show_licensed_features, NULL);

static void lm_qmi_svc_disconnect_cb(struct qmi_handle *qmi,
	unsigned int node, unsigned int port)
{
	struct client_info *itr, *tmp;

	if (!list_empty(&lm_svc->clients_connected)) {
		list_for_each_entry_safe(itr, tmp, &lm_svc->clients_connected,
								node) {
			if (itr->sq_node == node && itr->sq_port == port) {
				pr_info("Received LM QMI client disconnect "
					"from node:0x%x port:%d\n",
					node, port);
				list_del(&itr->node);
				kfree(itr);
			}
		}
	}
}

static struct qmi_ops lm_server_ops = {
	.del_client = lm_qmi_svc_disconnect_cb,
};
static struct qmi_msg_handler lm_req_handlers[] = {
	{
		.type = QMI_REQUEST,
		.msg_id = QMI_LM_FEATURE_LIST_REQ_V01,
		.ei = qmi_lm_feature_list_req_msg_v01_ei,
		.decoded_size = QMI_LM_FEATURE_LIST_REQ_MSG_V01_MAX_MSG_LEN,
		.fn = qmi_handle_feature_list_req,
	},
	{}
};

static int license_manager_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct lm_svc_ctx *svc;

	svc = kzalloc(sizeof(struct lm_svc_ctx), GFP_KERNEL);
	if (!svc)
		return -ENOMEM;

	svc->lm_svc_hdl = kzalloc(sizeof(struct qmi_handle), GFP_KERNEL);
	if (!svc->lm_svc_hdl) {
		ret = -ENOMEM;
		pr_err("%s:Mem allocation failed for LM svc handle %d\n",
							__func__, ret);
		goto free_lm_svc;
	}
	ret = qmi_handle_init(svc->lm_svc_hdl,
				QMI_LICENSE_MANAGER_SERVICE_MAX_MSG_LEN,
				&lm_server_ops,
				lm_req_handlers);
	if (ret < 0) {
		pr_err("%s:Error registering license manager svc %d\n",
							__func__, ret);
		goto free_lm_svc_handle;
	}
	ret = qmi_add_server(svc->lm_svc_hdl, QMI_LM_SERVICE_ID_V01,
					QMI_LM_SERVICE_VERS_V01,
					0);
	if (ret < 0) {
		pr_err("%s: failed to add license manager svc server :%d\n",
							__func__, ret);
		goto release_lm_svc_handle;
	}

	INIT_LIST_HEAD(&svc->clients_connected);
	INIT_LIST_HEAD(&svc->clients_feature_list);

	lm_svc = svc;

	/* Creating a directory in /sys/kernel/ */
	lm_kobj = kobject_create_and_add("license_manager", kernel_kobj);
	if (lm_kobj) {
		if (sysfs_create_file(lm_kobj, &lm_licensed_features_attr.attr)) {
			pr_err("Cannot create licensed_features sysfs file for lm\n");
			kobject_put(lm_kobj);
		}
	} else {
		pr_err("Unable to create license manager sysfs entry\n");
	}
	pr_info("License Manager driver registered\n");

	return 0;

release_lm_svc_handle:
	qmi_handle_release(svc->lm_svc_hdl);
free_lm_svc_handle:
	kfree(svc->lm_svc_hdl);
free_lm_svc:
	kfree(svc);

	return ret;
}

static int license_manager_remove(struct platform_device *pdev)
{
	struct lm_svc_ctx *svc = lm_svc;
	struct client_info *itr, *tmp;
	struct feature_info *iter, *temp;

	qmi_handle_release(svc->lm_svc_hdl);

	if (!list_empty(&svc->clients_connected)) {
		list_for_each_entry_safe(itr, tmp, &svc->clients_connected,
								node) {
			list_del(&itr->node);
			kfree(itr);
		}
	}
	if (!list_empty(&svc->clients_feature_list)) {
		list_for_each_entry_safe(iter, temp, &svc->clients_feature_list,
								node) {
			list_del(&iter->node);
			kfree(iter);
		}
	}

	kfree(svc->lm_svc_hdl);
	kfree(svc);

	lm_svc = NULL;

	return 0;
}

static const struct of_device_id of_license_manager_match[] = {
	{.compatible = "qti,license-manager-service"},
	{  /* sentinel value */ },
};

static struct platform_driver license_manager_driver = {
	.probe		= license_manager_probe,
	.remove		= license_manager_remove,
	.driver		= {
		.name	= "license_manager",
		.of_match_table	= of_license_manager_match,
	},
};

static int __init license_manager_init(void)
{
	return platform_driver_register(&license_manager_driver);
}
module_init(license_manager_init);

static void __exit license_manager_exit(void)
{
	platform_driver_unregister(&license_manager_driver);
}
module_exit(license_manager_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("License manager driver");
