// SPDX-License-Identifier: GPL-2.0-or-later
/* Native flowtable adapter shares the core's platform and power lifetime. */
#include <linux/platform_device.h>
#include "npu/flow.h"
#include "npu-flow/internal.h"
#include "npu-flow/net-event.h"
#include "npu-flow/tnl-offload.h"

struct npu_flow_dev npu_flow;
static bool flow_started;

int mtk_npu_flow_start(struct platform_device *pdev)
{
	int ret;

	npu_flow.dev = &pdev->dev;
	ret = mtk_npu_flow_tnl_offload_init();
	if (ret)
		return ret;
	ret = mtk_npu_flow_net_event_init();
	if (ret)
		goto finish;
	ret = mtk_npu_flow_tnl_start();
	if (ret)
		goto event_remove;
	flow_started = true;
	return 0;
event_remove:
	mtk_npu_flow_net_event_deinit();
finish:
	mtk_npu_flow_tnl_offload_deinit();
	return ret;
}

void mtk_npu_flow_quiesce(void)
{
	if (!flow_started)
		return;
	mtk_npu_flow_tnl_stop();
	mtk_npu_flow_net_event_deinit();
}

void mtk_npu_flow_finish(void)
{
	if (!flow_started)
		return;
	mtk_npu_flow_tnl_offload_deinit();
	flow_started = false;
	npu_flow.dev = NULL;
}
