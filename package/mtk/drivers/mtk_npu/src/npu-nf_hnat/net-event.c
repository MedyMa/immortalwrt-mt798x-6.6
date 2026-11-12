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

#include "npu/netsys.h"
#include "npu/ser.h"
#include "npu/tunnel.h"
#include "npu/trm.h"

#include "npu-nf_hnat/internal.h"
#include "npu-nf_hnat/net-event.h"
#include "npu-nf_hnat/tnl-offload.h"

static struct completion wait_fe_reset_done;
/* RTNL protects a single FE transaction, including stop-to-reset fallback. */
static bool fe_reset_pending;

static void npu_reset_client_notify(unsigned long event)
{
	struct mtk_eth *eth = mtk_npu_netsys_get_eth();
	int i;

	if (!eth)
		return;
	rtnl_lock();
	for (i = 0; i < MTK_MAX_DEVS; i++) {
		if (!eth->netdev[i])
			continue;
		call_netdevice_notifiers(event, eth->netdev[i]);
		break;
	}
	rtnl_unlock();
}

static void mtk_npu_nf_hnat_netdev_ser_callback(struct npu_ser_params *ser_param)
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
	if (!wait_for_completion_timeout(&wait_fe_reset_done, 30 * HZ)) {
		NPU_WARN("FE reset completion timed out\n");
		/* Retract the quiesce ACK before firmware is allowed to restart. */
		rtnl_lock();
		call_netdevice_notifiers(MTK_TOPS_RESET_FAILED, netdev);
		rtnl_unlock();
	}
}

static int mtk_npu_nf_hnat_netdev_ser(struct net_device *dev)
{
	struct npu_ser_params ser_params = {
		.type = NPU_SER_NETSYS_FE_RST,
		.data.net.ndev = dev,
		.ser_callback = mtk_npu_nf_hnat_netdev_ser_callback,
	};

	/* Each FE reset must wait for its own completion, not a previous cycle. */
	reinit_completion(&wait_fe_reset_done);
	return mtk_npu_ser(&ser_params);
}


/* Device topology, address and MTU changes invalidate cached templates. */
static int mtk_npu_nf_hnat_netdev_callback(struct notifier_block *nb,
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
		/* A fallback request belongs to the same stopped firmware cycle. */
		if (fe_reset_pending)
			break;
		ret = mtk_npu_nf_hnat_netdev_ser(dev);
		fe_reset_pending = !ret;
		if (ret)
			NPU_WARN("could not queue FE reset recovery: %d\n", ret);
		break;
	case MTK_FE_RESET_DONE:
	case MTK_FE_START_TRAFFIC:
		fe_reset_pending = false;
		complete(&wait_fe_reset_done);
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block mtk_npu_netdev_notifier = {
	.notifier_call = mtk_npu_nf_hnat_netdev_callback,
};

int mtk_npu_nf_hnat_net_event_init(void)
{
	int ret = 0;

	init_completion(&wait_fe_reset_done);
	fe_reset_pending = false;
	ret = register_netdevice_notifier(&mtk_npu_netdev_notifier);
	if (ret) {
		NPU_NOTICE("NPU register netdev notifier failed: %d\n", ret);
		return ret;
	}

	npu_reset_client_notify(MTK_TOPS_ONLINE);
	return ret;
}

void mtk_npu_nf_hnat_net_event_deinit(void)
{
	npu_reset_client_notify(MTK_TOPS_OFFLINE);
	unregister_netdevice_notifier(&mtk_npu_netdev_notifier);
	/* A queued SER callback must not wait for an unregistered notifier. */
	complete_all(&wait_fe_reset_done);
}
