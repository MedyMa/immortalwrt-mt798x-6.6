/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _NPU_DPDK_H_
#define _NPU_DPDK_H_

#include <linux/ioport.h>
#include <linux/platform_device.h>

#if IS_ENABLED(CONFIG_MTK_NPU_DPDK)
int mtk_npu_dpdk_start(struct platform_device *pdev, void __iomem *base,
		       resource_size_t size);
void mtk_npu_dpdk_stop(void);
#else
static inline int mtk_npu_dpdk_start(struct platform_device *pdev,
				   void __iomem *base, resource_size_t size)
{
	return 0;
}
static inline void mtk_npu_dpdk_stop(void) {}
#endif
#endif
