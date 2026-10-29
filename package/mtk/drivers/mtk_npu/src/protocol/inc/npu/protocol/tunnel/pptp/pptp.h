/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2025 MediaTek Inc. All Rights Reserved.
 *
 * Author: Frank-zj Lin <frank-zj.lin@mediatek.com>
 */

#ifndef _NPU_PPTP_H_
#define _NPU_PPTP_H_

#include <linux/types.h>

struct npu_tnl_type;
struct npu_tnl_input;
struct npu_tnl_info;
struct npu_params;

int mtk_npu_pptp_seq_init(struct npu_tnl_type *type);
void mtk_npu_pptp_seq_deinit(void);
int mtk_npu_pptp_session_prepare(struct npu_tnl_input *input, struct npu_params *params);
int mtk_npu_pptp_session_get(struct npu_tnl_info *info, u64 cookie, struct npu_params *params);
void mtk_npu_pptp_session_put(struct npu_tnl_info *info);

#if defined(CONFIG_MTK_NPU_PPTP)
int mtk_npu_pptp_init(void);
void mtk_npu_pptp_deinit(void);
void mtk_npu_pptp_seq_suspend_stalled(void);
void mtk_npu_pptp_seq_resume_stalled(void);
#else /* !defined(CONFIG_MTK_NPU_PPTP) */
static inline int mtk_npu_pptp_init(void)
{
	return 0;
}

static inline void mtk_npu_pptp_deinit(void)
{
}
static inline void mtk_npu_pptp_seq_suspend_stalled(void) { }
static inline void mtk_npu_pptp_seq_resume_stalled(void) { }
#endif /* defined(CONFIG_MTK_NPU_PPTP) */
#endif /* _NPU_PPTP_H_ */
