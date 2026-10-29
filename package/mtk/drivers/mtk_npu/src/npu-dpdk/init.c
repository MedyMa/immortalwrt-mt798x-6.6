// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2026 MediaTek Inc. All Rights Reserved. */

#include <linux/err.h>
#include <linux/rwsem.h>
#include <linux/mutex.h>

#include "npu/dpdk.h"
#include "npu/mcu.h"
#include "npu/net-core.h"
#include "npu-dpdk/cdev.h"
#include "npu-dpdk/internal.h"
#include "npu-dpdk/mac-filter.h"
#include "npu-dpdk/npu-dpdk-cmd.h"
#include "npu-dpdk/procfs.h"
#include "npu-dpdk/sysfs.h"

struct npu_dpdk_dev ndpdk;
static DECLARE_RWSEM(dpdk_access);
static DEFINE_MUTEX(dpdk_lifecycle);
static unsigned long dpdk_generation;
static enum {
	DPDK_DOWN, DPDK_STARTING, DPDK_READY, DPDK_RECOVERING, DPDK_FAILED
} dpdk_state;

int mtk_npu_dpdk_access_begin(void)
{
	down_read(&dpdk_access);
	if (dpdk_state != DPDK_READY || !mtk_npu_mcu_alive()) {
		up_read(&dpdk_access);
		return -ENODEV;
	}
	return 0;
}

void mtk_npu_dpdk_access_end(void)
{
	up_read(&dpdk_access);
}

unsigned long mtk_npu_dpdk_generation(void)
{
	return dpdk_generation;
}

static int mtk_npu_dpdk_enable(bool en)
{
	struct net_cmd cmd = {
		.type = NPU_NET_CMD_TYPE_DPDK,
		.sub_type = NPU_DPDK_NET_CMD_SET_EN,
		.arg[0] = en,
	};

	return mtk_npu_net_send_cmd_all_no_wait(&cmd);
}

static int mtk_npu_dpdk_before_reset(struct mcu_state_notifier *notifier)
{
	down_write(&dpdk_access);
	if (dpdk_state == DPDK_READY || dpdk_state == DPDK_FAILED)
		dpdk_state = DPDK_RECOVERING;
	up_write(&dpdk_access);
	return 0;
}

static int mtk_npu_dpdk_after_reset(struct mcu_state_notifier *notifier)
{
	int ret = 0;

	down_write(&dpdk_access);
	if (dpdk_state != DPDK_RECOVERING)
		goto out;
	ret = mtk_npu_dpdk_enable(true);
	if (!ret)
		ret = mtk_npu_dpdk_mac_filter_restore();
	if (ret) {
		mtk_npu_dpdk_enable(false);
		dpdk_state = DPDK_FAILED;
		NPU_ERR("firmware state replay failed: %d\n", ret);
	} else {
		dpdk_state = DPDK_READY;
	}
out:
	up_write(&dpdk_access);
	return ret;
}

static struct mcu_state_notifier dpdk_stop_notifier = {
	.pre_enter = mtk_npu_dpdk_before_reset,
};
static struct mcu_state_notifier dpdk_init_notifier = {
	.post_enter = mtk_npu_dpdk_after_reset,
};
static struct mcu_state_notifier dpdk_freerun_notifier = {
	/* A warm reset may return from NETSTOP without passing through INIT. */
	.pre_enter = mtk_npu_dpdk_after_reset,
};

int mtk_npu_dpdk_start(struct platform_device *pdev, void __iomem *base,
		       resource_size_t size)
{
	int ret;

	if (!pdev || !base || !size)
		return -EINVAL;
	mutex_lock(&dpdk_lifecycle);
	down_write(&dpdk_access);
	if (dpdk_state != DPDK_DOWN) {
		ret = -EBUSY;
		goto out;
	}
	ndpdk.dev = &pdev->dev;
	ndpdk.base = base;
	ndpdk.size = size;
	dpdk_state = DPDK_STARTING;
	ret = mtk_npu_dpdk_enable(true);
	if (ret)
		goto disable;
	ret = mtk_npu_dpdk_mac_filter_init();
	if (ret)
		goto disable;
	up_write(&dpdk_access);

	ret = mtk_npu_mcu_state_notifier_register(MCU_STATE_TYPE_NETSTOP,
						 &dpdk_stop_notifier);
	if (ret)
		goto filter_deinit;
	ret = mtk_npu_mcu_state_notifier_register(MCU_STATE_TYPE_INIT,
						 &dpdk_init_notifier);
	if (ret)
		goto unregister_stop;
	ret = mtk_npu_mcu_state_notifier_register(MCU_STATE_TYPE_FREERUN,
						 &dpdk_freerun_notifier);
	if (ret)
		goto unregister_init;
	ret = mtk_npu_dpdk_procfs_init();
	if (ret)
		goto unregister_freerun;
	ret = mtk_npu_dpdk_sysfs_init();
	if (ret)
		goto procfs_deinit;
	/* Publish the ioctl interface last. Opens remain blocked until READY. */
	down_write(&dpdk_access);
	dpdk_generation++;
	up_write(&dpdk_access);
	ret = mtk_npu_dpdk_cdev_init();
	if (ret)
		goto sysfs_deinit;

	down_write(&dpdk_access);
	/* Re-fetch the firmware address after notifier registration as a reset
	 * may have occurred during setup. No user operation has been admitted.
	 */
	if (!mtk_npu_mcu_alive())
		ret = -EBUSY;
	else
		ret = mtk_npu_dpdk_enable(true);
	if (!ret)
		ret = mtk_npu_dpdk_mac_filter_restore();
	if (!ret)
		dpdk_state = DPDK_READY;
	up_write(&dpdk_access);
	if (!ret) {
		mutex_unlock(&dpdk_lifecycle);
		return 0;
	}
	mtk_npu_dpdk_cdev_deinit();
sysfs_deinit:
	mtk_npu_dpdk_sysfs_deinit();
procfs_deinit:
	mtk_npu_dpdk_procfs_deinit();
unregister_freerun:
	mtk_npu_mcu_state_notifier_unregister(MCU_STATE_TYPE_FREERUN, &dpdk_freerun_notifier);
unregister_init:
	mtk_npu_mcu_state_notifier_unregister(MCU_STATE_TYPE_INIT, &dpdk_init_notifier);
unregister_stop:
	mtk_npu_mcu_state_notifier_unregister(MCU_STATE_TYPE_NETSTOP, &dpdk_stop_notifier);
filter_deinit:
	down_write(&dpdk_access);
	mtk_npu_dpdk_mac_filter_deinit();
disable:
	mtk_npu_dpdk_enable(false);
	ndpdk.dev = NULL;
	ndpdk.base = NULL;
	ndpdk.size = 0;
	dpdk_state = DPDK_DOWN;
out:
	up_write(&dpdk_access);
	mutex_unlock(&dpdk_lifecycle);
	return ret;
}

void mtk_npu_dpdk_stop(void)
{
	mutex_lock(&dpdk_lifecycle);
	down_write(&dpdk_access);
	if (dpdk_state == DPDK_DOWN) {
		up_write(&dpdk_access);
		mutex_unlock(&dpdk_lifecycle);
		return;
	}
	/* Drain readers and deny existing fds before removing their endpoints. */
	dpdk_state = DPDK_DOWN;
	up_write(&dpdk_access);
	/* Callbacks take dpdk_access: never unregister while holding it. */
	mtk_npu_mcu_state_notifier_unregister(MCU_STATE_TYPE_FREERUN, &dpdk_freerun_notifier);
	mtk_npu_mcu_state_notifier_unregister(MCU_STATE_TYPE_INIT, &dpdk_init_notifier);
	mtk_npu_mcu_state_notifier_unregister(MCU_STATE_TYPE_NETSTOP, &dpdk_stop_notifier);
	mtk_npu_dpdk_cdev_deinit();
	mtk_npu_dpdk_sysfs_deinit();
	mtk_npu_dpdk_procfs_deinit();
	down_write(&dpdk_access);
	mtk_npu_dpdk_mac_filter_deinit();
	mtk_npu_dpdk_enable(false);
	ndpdk.dev = NULL;
	ndpdk.base = NULL;
	ndpdk.size = 0;
	up_write(&dpdk_access);
	mutex_unlock(&dpdk_lifecycle);
}
