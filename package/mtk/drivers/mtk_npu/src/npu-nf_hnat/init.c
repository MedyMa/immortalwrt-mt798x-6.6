// SPDX-License-Identifier: GPL-2.0-or-later
/* The 24.10 adapter shares NPU's platform, device links and worker lifetime. */
#include <linux/platform_device.h>
#include "npu/nf-hnat.h"
#include "npu-nf_hnat/internal.h"
#include "npu-nf_hnat/net-event.h"
#include "npu-nf_hnat/statistic.h"
#include "npu-nf_hnat/tnl-offload.h"

struct npu_nf_hnat_dev npu_nf_hnat;
static bool started;

int mtk_npu_nf_hnat_start(struct platform_device *pdev)
{
	int ret;

	npu_nf_hnat.dev = &pdev->dev;
	ret = mtk_npu_nf_hnat_tnl_offload_init();
	if (ret)
		return ret;
	ret = mtk_npu_nf_hnat_net_event_init();
	if (ret)
		goto finish;
	ret = mtk_npu_nf_hnat_statistic_init();
	if (ret)
		goto event_stop;
	/* Publish packet entry points last; no fallible work follows success. */
	ret = mtk_npu_nf_hnat_tnl_offload_start();
	if (ret)
		goto stat_stop;
	started = true;
	return 0;
stat_stop:
	mtk_npu_nf_hnat_statistic_deinit();
event_stop:
	mtk_npu_nf_hnat_net_event_deinit();
finish:
	mtk_npu_nf_hnat_tnl_offload_deinit();
	return ret;
}

void mtk_npu_nf_hnat_quiesce(void)
{
	if (!started)
		return;
	mtk_npu_nf_hnat_tnl_offload_stop();
	mtk_npu_nf_hnat_net_event_deinit();
	mtk_npu_nf_hnat_statistic_deinit();
}

void mtk_npu_nf_hnat_finish(void)
{
	if (!started)
		return;
	mtk_npu_nf_hnat_tnl_offload_deinit();
	started = false;
}
