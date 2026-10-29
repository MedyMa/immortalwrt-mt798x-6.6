// SPDX-License-Identifier: GPL-2.0-or-later
/* Keep firmware templates consistent with the host IPv4 route/ARP policy. */
#include <linux/inetdevice.h>
#include <linux/notifier.h>
#include <net/fib_notifier.h>
#include <net/netevent.h>
#include <net/neighbour.h>
#include <net/net_namespace.h>

#include "npu/internal.h"
#include "npu/npu.h"
#include "npu/net-event.h"
#include "npu/tunnel.h"

static int mtk_npu_netevent_callback(struct notifier_block *nb,
				     unsigned long event, void *data)
{
	struct neighbour *neigh = data;
	u8 ha[ETH_ALEN];

	if (event != NETEVENT_NEIGH_UPDATE || !mtk_npu_is_ready() ||
	    !neigh || !neigh->dev || dev_net(neigh->dev) != &init_net ||
	    neigh->tbl->family != AF_INET)
		return NOTIFY_DONE;
	neigh_ha_snapshot(ha, neigh, neigh->dev);
	mtk_npu_tnl_info_invalidate_neigh(neigh->dev,
		READ_ONCE(neigh->nud_state) & NUD_VALID ? ha : NULL);
	return NOTIFY_DONE;
}

static int mtk_npu_inetaddr_callback(struct notifier_block *nb,
				    unsigned long event, void *data)
{
	struct in_ifaddr *ifa = data;

	if (mtk_npu_is_ready() && (event == NETDEV_UP || event == NETDEV_DOWN) &&
	    ifa && ifa->ifa_dev && dev_net(ifa->ifa_dev->dev) == &init_net)
		mtk_npu_tnl_info_invalidate_netdev(ifa->ifa_dev->dev);
	return NOTIFY_DONE;
}

static int mtk_npu_fib_callback(struct notifier_block *nb,
			       unsigned long event, void *data)
{
	struct fib_notifier_info *info = data;

	if (!mtk_npu_is_ready() || !info || info->family != AF_INET)
		return NOTIFY_DONE;
	switch (event) {
	case FIB_EVENT_ENTRY_REPLACE:
	case FIB_EVENT_ENTRY_APPEND:
	case FIB_EVENT_ENTRY_ADD:
	case FIB_EVENT_ENTRY_DEL:
	case FIB_EVENT_RULE_ADD:
	case FIB_EVENT_RULE_DEL:
	case FIB_EVENT_NH_ADD:
	case FIB_EVENT_NH_DEL:
		/* Route/rule changes may affect any cached tunnel on init_net. */
		mtk_npu_tnl_info_invalidate_netdev(NULL);
		break;
	default:
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block mtk_npu_netevent_notifier = {
	.notifier_call = mtk_npu_netevent_callback,
};
static struct notifier_block mtk_npu_inetaddr_notifier = {
	.notifier_call = mtk_npu_inetaddr_callback,
};
static struct notifier_block mtk_npu_fib_notifier = {
	.notifier_call = mtk_npu_fib_callback,
};

int mtk_npu_netevent_register(struct platform_device *pdev)
{
	int ret;

	ret = register_netevent_notifier(&mtk_npu_netevent_notifier);
	if (ret)
		return ret;
	ret = register_inetaddr_notifier(&mtk_npu_inetaddr_notifier);
	if (ret)
		goto netevent_remove;
	ret = register_fib_notifier(&init_net, &mtk_npu_fib_notifier, NULL, NULL);
	if (!ret)
		return 0;
	unregister_inetaddr_notifier(&mtk_npu_inetaddr_notifier);
netevent_remove:
	unregister_netevent_notifier(&mtk_npu_netevent_notifier);
	return ret;
}

void mtk_npu_netevent_unregister(struct platform_device *pdev)
{
	unregister_fib_notifier(&init_net, &mtk_npu_fib_notifier);
	unregister_inetaddr_notifier(&mtk_npu_inetaddr_notifier);
	unregister_netevent_notifier(&mtk_npu_netevent_notifier);
}
