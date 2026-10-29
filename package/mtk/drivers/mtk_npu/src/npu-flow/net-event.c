// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 MediaTek Inc. All Rights Reserved.
 *
 * Author: Ren-Ting Wang <ren-ting.wang@mediatek.com>
 */

#include <linux/completion.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>

#include <mtk_eth_soc.h>
#include <mtk_eth_reset.h>

#include "npu/ser.h"
#include "npu/tunnel.h"
#include "npu/trm.h"

#include "npu-flow/internal.h"
#include "npu-flow/net-event.h"
#include "npu-flow/tnl-offload.h"

static struct completion wait_fe_reset_done;

static void mtk_npu_flow_netdev_ser_callback(struct npu_ser_params *ser_param)
{
	struct net_device *netdev = ser_param->data.net.ndev;

	WARN_ON(ser_param->type != NPU_SER_NETSYS_FE_RST);

	mtk_trm_dump(TRM_RSN_FE_RESET);

	/* send npu dump done notification to mtk eth */
	rtnl_lock();
	call_netdevice_notifiers(MTK_TOPS_DUMP_DONE, netdev);
	rtnl_unlock();

	/* wait for FE reset done notification */
	/* TODO : if not received FE reset done notification */
	if (!wait_for_completion_timeout(&wait_fe_reset_done, 30 * HZ))
		NPU_WARN("FE reset completion timed out\n");
}

static void mtk_npu_flow_netdev_ser(struct net_device *dev)
{
	struct npu_ser_params ser_params = {
		.type = NPU_SER_NETSYS_FE_RST,
		.data.net.ndev = dev,
		.ser_callback = mtk_npu_flow_netdev_ser_callback,
	};

	/* Each FE reset must wait for its own completion, not a previous cycle. */
	reinit_completion(&wait_fe_reset_done);
	mtk_npu_ser(&ser_params);
}


/* Device topology, address and MTU changes invalidate cached templates. */
static int mtk_npu_flow_netdev_callback(struct notifier_block *nb,
					   unsigned long event,
					   void *data)
{
	struct net_device *dev = netdev_notifier_info_to_dev(data);
	int ret = 0;

	switch (event) {
	case NETDEV_DOWN:
	case NETDEV_UNREGISTER:
	case NETDEV_CHANGE:
	case NETDEV_CHANGEADDR:
	case NETDEV_CHANGEMTU:
	case NETDEV_CHANGEUPPER:
		mtk_npu_tnl_info_invalidate_netdev(dev);
		break;
	case MTK_FE_START_RESET:
	case MTK_FE_STOP_TRAFFIC:
		mtk_npu_flow_netdev_ser(dev);
		break;
	case MTK_FE_RESET_DONE:
	case MTK_FE_START_TRAFFIC:
		complete(&wait_fe_reset_done);
		break;
	default:
		break;
	}

	return ret;
}

static struct notifier_block mtk_npu_netdev_notifier = {
	.notifier_call = mtk_npu_flow_netdev_callback,
};

int mtk_npu_flow_net_event_init(void)
{
	int ret = 0;

	init_completion(&wait_fe_reset_done);
	ret = register_netdevice_notifier(&mtk_npu_netdev_notifier);
	if (ret) {
		NPU_NOTICE("NPU register netdev notifier failed: %d\n", ret);
		return ret;
	}

	return ret;
}

void mtk_npu_flow_net_event_deinit(void)
{
	unregister_netdevice_notifier(&mtk_npu_netdev_notifier);
	/* A queued SER callback must not wait for an unregistered notifier. */
	complete_all(&wait_fe_reset_done);
}
