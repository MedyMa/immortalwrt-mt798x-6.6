// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2026 MediaTek Inc. All Rights Reserved.
 *
 */

#include <linux/capability.h>
#include "npu/sysfs.h"

#include "npu-dpdk/internal.h"
#include "npu-dpdk/mac-filter.h"
#include "npu-dpdk/sysfs.h"

static struct kset *net_kset;

static ssize_t dpdk_mac_filter_enable_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	u32 val;
	int ret;

	if (!capable(CAP_NET_ADMIN))
		return -EPERM;
	if (kstrtou32(buf, 10, &val) || val > 1)
		return -EINVAL;

	ret = mtk_npu_dpdk_access_begin();
	if (ret)
		return ret;
	ret = mtk_npu_dpdk_mac_filter_enable(val);
	mtk_npu_dpdk_access_end();
	return ret ? ret : count;
}

static NPU_DEV_ATTR_WO(dpdk, mac_filter_enable);

static struct attribute *dpdk_attributes[] = {
	&dev_attr_dpdk_mac_filter_enable.attr,
	NULL,
};

static const struct attribute_group dpdk_attr_group = {
	.name = "dpdk",
	.attrs = dpdk_attributes,
};

int mtk_npu_dpdk_sysfs_init(void)
{
	struct kobject *kobj;
	int ret;

	kobj = kset_find_obj(npu_kset, "net");
	if (!kobj) {
		NPU_NOTICE("npu/net/ sysfs is not properly installed\n");
		return -ENOENT;
	}

	net_kset = to_kset(kobj);

	ret = sysfs_create_group(&net_kset->kobj, &dpdk_attr_group);
	if (ret) {
		NPU_NOTICE("create npu/net/dpdk sysfs failed\n");
		kobject_put(kobj);
		net_kset = NULL;
		return ret;
	}

	return ret;
}

void mtk_npu_dpdk_sysfs_deinit(void)
{
	sysfs_remove_group(&net_kset->kobj, &dpdk_attr_group);
	kobject_put(&net_kset->kobj);
	net_kset = NULL;
}
