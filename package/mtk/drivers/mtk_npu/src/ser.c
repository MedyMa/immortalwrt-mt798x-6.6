// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 MediaTek Inc. All Rights Reserved.
 *
 * Author: Alvin Kuo <alvin.kuog@mediatek.com>
 */

#include <linux/err.h>
#include <linux/workqueue.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <net/flow_offload.h>

#include "npu/internal.h"
#include "npu/net-event.h"
#include "npu/ser.h"
#include "npu/trm.h"

struct npu_ser {
	struct work_struct work;
	struct npu_ser_params ser_params;
	spinlock_t params_lock;
	u64 generation;
	bool stopping;
};

static struct npu_ser npu_ser = {
	.params_lock = __SPIN_LOCK_UNLOCKED(npu_ser.params_lock),
	.stopping = true,
};

static void mtk_npu_ser_params_get(struct npu_ser_params *params)
{
	if (params->type == NPU_SER_NETSYS_FE_RST)
		dev_hold(params->data.net.ndev);
}

static void mtk_npu_ser_params_put(struct npu_ser_params *params)
{
	if (params->type == NPU_SER_NETSYS_FE_RST)
		dev_put(params->data.net.ndev);
}

static inline void __mtk_npu_ser_cmd_clear(void)
{
	mtk_npu_ser_params_put(&npu_ser.ser_params);
	memset(&npu_ser.ser_params, 0, sizeof(struct npu_ser_params));
	npu_ser.ser_params.type = __NPU_SER_TYPE_MAX;
}

static void mtk_npu_ser_setup_mcmd(struct npu_ser_params *ser_params,
				   struct mcu_ctrl_cmd *mcmd)
{
	memset(mcmd, 0, sizeof(struct mcu_ctrl_cmd));

	switch (ser_params->type) {
	case NPU_SER_NETSYS_FE_RST:
		mcmd->e = MCU_EVENT_TYPE_FE_RESET;
		break;
	case NPU_SER_WDT_TO:
		mcmd->e = MCU_EVENT_TYPE_WDT_TIMEOUT;
		break;
	default:
		NPU_ERR("unsupport NPU SER type: %u\n", ser_params->type);
		return;
	}

	if (ser_params->ser_mcmd_setup)
		ser_params->ser_mcmd_setup(ser_params, mcmd);
}

static void mtk_npu_ser_reset_callback(void *params)
{
	struct npu_ser_params *ser_params = params;

	if (ser_params->ser_callback)
		ser_params->ser_callback(ser_params);
}

static void mtk_npu_ser_work(struct work_struct *work)
{
	struct npu_ser_params ser_params;
	struct mcu_ctrl_cmd mcmd;
	unsigned long flag = 0;
	u64 generation;
	int ret;

	spin_lock_irqsave(&npu_ser.params_lock, flag);

	while (!npu_ser.stopping &&
	       npu_ser.ser_params.type != __NPU_SER_TYPE_MAX) {
		memcpy(&ser_params,
		       &npu_ser.ser_params,
		       sizeof(struct npu_ser_params));
		generation = npu_ser.generation;
		/* A replacement request owns a separate reference from this one. */
		mtk_npu_ser_params_get(&ser_params);

		spin_unlock_irqrestore(&npu_ser.params_lock, flag);

		mtk_npu_ser_setup_mcmd(&ser_params, &mcmd);

		ret = mtk_npu_mcu_reset(&mcmd, mtk_npu_ser_reset_callback,
				    &ser_params);
		mtk_npu_ser_params_put(&ser_params);
		if (ret) {
			NPU_INFO("SER type: %u failed to recover\n",
				  ser_params.type);
		} else {
			NPU_INFO("SER type: %u successfully recovered\n",
				  ser_params.type);
		}

		spin_lock_irqsave(&npu_ser.params_lock, flag);
		/* Do not discard a newer request, including an identical FE reset. */
		if (npu_ser.generation == generation)
			__mtk_npu_ser_cmd_clear();
	}

	spin_unlock_irqrestore(&npu_ser.params_lock, flag);
}

int mtk_npu_ser(struct npu_ser_params *ser_params)
{
	unsigned long flag;
	int ret = 0;

	if (!ser_params || (unsigned int)ser_params->type >= __NPU_SER_TYPE_MAX ||
	    (ser_params->type == NPU_SER_NETSYS_FE_RST && !ser_params->data.net.ndev))
		return -EINVAL;

	spin_lock_irqsave(&npu_ser.params_lock, flag);

	if (npu_ser.stopping) {
		ret = -ESHUTDOWN;
		goto unlock;
	}

	/* higher SER type should not override lower SER type */
	if (npu_ser.ser_params.type != __NPU_SER_TYPE_MAX
	    && npu_ser.ser_params.type < ser_params->type)
		goto unlock;

	mtk_npu_ser_params_get(ser_params);
	__mtk_npu_ser_cmd_clear();
	memcpy(&npu_ser.ser_params, ser_params, sizeof(*ser_params));
	npu_ser.generation++;

	schedule_work(&npu_ser.work);

unlock:
	spin_unlock_irqrestore(&npu_ser.params_lock, flag);

	return ret;
}
EXPORT_SYMBOL(mtk_npu_ser);

int mtk_npu_ser_init(struct platform_device *pdev)
{
	INIT_WORK(&npu_ser.work, mtk_npu_ser_work);

	spin_lock_init(&npu_ser.params_lock);

	npu_ser.ser_params.type = __NPU_SER_TYPE_MAX;
	npu_ser.generation = 0;
	npu_ser.stopping = false;

	return 0;
}

int mtk_npu_ser_deinit(struct platform_device *pdev)
{
	unsigned long flag;

	spin_lock_irqsave(&npu_ser.params_lock, flag);
	npu_ser.stopping = true;
	__mtk_npu_ser_cmd_clear();
	spin_unlock_irqrestore(&npu_ser.params_lock, flag);

	cancel_work_sync(&npu_ser.work);

	return 0;
}
