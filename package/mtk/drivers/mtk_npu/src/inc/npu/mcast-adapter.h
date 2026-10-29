/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _NPU_MCAST_ADAPTER_H_
#define _NPU_MCAST_ADAPTER_H_
#include <linux/platform_device.h>
#if IS_ENABLED(CONFIG_MTK_NPU_MCAST)
int mtk_npu_mcast_start(struct platform_device *pdev, void __iomem *base);
void mtk_npu_mcast_stop(void);
#else
static inline int mtk_npu_mcast_start(struct platform_device *pdev, void __iomem *base)
{
	return 0;
}
static inline void mtk_npu_mcast_stop(void) { }
#endif
#endif
