/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _NPU_NF_HNAT_ADAPTER_H_
#define _NPU_NF_HNAT_ADAPTER_H_
#include <linux/platform_device.h>
#if IS_ENABLED(CONFIG_MTK_NPU_HNAT)
int mtk_npu_nf_hnat_start(struct platform_device *pdev);
void mtk_npu_nf_hnat_quiesce(void);
void mtk_npu_nf_hnat_finish(void);
#else
static inline int mtk_npu_nf_hnat_start(struct platform_device *pdev) { return 0; }
static inline void mtk_npu_nf_hnat_quiesce(void) { }
static inline void mtk_npu_nf_hnat_finish(void) { }
#endif
#endif
