/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _NPU_FLOW_ADAPTER_H_
#define _NPU_FLOW_ADAPTER_H_
#include <linux/platform_device.h>
#if IS_ENABLED(CONFIG_MTK_NPU_FLOW)
int mtk_npu_flow_start(struct platform_device *pdev);
void mtk_npu_flow_quiesce(void);
void mtk_npu_flow_finish(void);
#else
static inline int mtk_npu_flow_start(struct platform_device *pdev) { return 0; }
static inline void mtk_npu_flow_quiesce(void) { }
static inline void mtk_npu_flow_finish(void) { }
#endif
#endif
