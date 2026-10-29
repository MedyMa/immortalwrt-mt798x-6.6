/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2026 MediaTek Inc. All Rights Reserved.
 *
 * Author: Ren-Ting Wang <ren-ting.wang@mediatek.com>
 */

#ifndef _NPU_DPDK_INTERNAL_H_
#define _NPU_DPDK_INTERNAL_H_

#include <linux/device.h>
#include <linux/ioport.h>

struct npu_dpdk_dev {
	struct device *dev;
	void __iomem *base;
	resource_size_t size;
};

extern struct npu_dpdk_dev ndpdk;

int mtk_npu_dpdk_access_begin(void);
void mtk_npu_dpdk_access_end(void);
/* Caller holds dpdk_access through mtk_npu_dpdk_access_begin(). */
unsigned long mtk_npu_dpdk_generation(void);

#define NPU_DBG(fmt, ...)		dev_dbg(ndpdk.dev, "dpdk: " fmt, ##__VA_ARGS__)
#define NPU_INFO(fmt, ...)		dev_info(ndpdk.dev, "dpdk: " fmt, ##__VA_ARGS__)
#define NPU_NOTICE(fmt, ...)		dev_notice(ndpdk.dev, "dpdk: " fmt, ##__VA_ARGS__)
#define NPU_WARN(fmt, ...)		dev_warn(ndpdk.dev, "dpdk: " fmt, ##__VA_ARGS__)
#define NPU_ERR(fmt, ...)		dev_err(ndpdk.dev, "dpdk: " fmt, ##__VA_ARGS__)
#endif /* _NPU_DPDK_INTERNAL_H_ */
