// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (c) 2025 MediaTek Inc. All Rights Reserved. */
#include <linux/mutex.h>

#include "npu/mcast-adapter.h"
#include "npu/mcu.h"
#include "npu-mcast/internal.h"
#include "npu-mcast/mcast.h"
#include "npu-mcast/procfs.h"
#include "npu-mcast/sysfs.h"

struct npu_mcast_dev nmcast;
static DEFINE_MUTEX(mcast_lifecycle);
static DEFINE_MUTEX(mcast_state_lock);
static enum { MCAST_DOWN, MCAST_STARTING, MCAST_UP, MCAST_RECOVERING } mcast_state;

static int mcast_before_reset(struct mcu_state_notifier *notifier)
{
	mutex_lock(&mcast_state_lock);
	if (mcast_state != MCAST_DOWN) {
		/* Deny new bindings and drain existing PPE entries before reset. */
		mtk_npu_mcast_pause();
		mcast_state = MCAST_RECOVERING;
	}
	mutex_unlock(&mcast_state_lock);
	return 0;
}

static int mcast_after_reset(struct mcu_state_notifier *notifier)
{
	int ret = 0;

	mutex_lock(&mcast_state_lock);
	if (mcast_state == MCAST_RECOVERING) {
		ret = mtk_npu_mcast_restore();
		if (!ret) {
			mcast_state = MCAST_UP;
			mtk_npu_mcast_resume();
		}
	}
	mutex_unlock(&mcast_state_lock);
	return ret;
}

static struct mcu_state_notifier mcast_stop_notifier = {
	.pre_enter = mcast_before_reset,
};
static struct mcu_state_notifier mcast_init_notifier = {
	.post_enter = mcast_after_reset,
};
static struct mcu_state_notifier mcast_freerun_notifier = {
	/* Warm reset can return from NETSTOP without passing through INIT. */
	.pre_enter = mcast_after_reset,
};

int mtk_npu_mcast_enable(bool en)
{
	int ret = -EAGAIN;

	mutex_lock(&mcast_state_lock);
	if (mcast_state == MCAST_UP && mtk_npu_mcu_alive()) {
		/* Drain PPE before disabling firmware replication. Re-enabling
		 * rebuilds membership before any new hardware binding is allowed.
		 */
		mtk_npu_mcast_pause();
		ret = mtk_npu_mcast_set_enabled(en);
		if (!ret)
			mtk_npu_mcast_resume();
	}
	mutex_unlock(&mcast_state_lock);
	return ret;
}

int mtk_npu_mcast_start(struct platform_device *pdev, void __iomem *base)
{
	int ret;

	if (!pdev || !base)
		return -EINVAL;
	mutex_lock(&mcast_lifecycle);
	mutex_lock(&mcast_state_lock);
	if (mcast_state != MCAST_DOWN) {
		ret = -EBUSY;
		mutex_unlock(&mcast_state_lock);
		goto out;
	}
	nmcast.dev = &pdev->dev;
	nmcast.base = base;
	mcast_state = MCAST_STARTING;
	ret = mtk_npu_mcast_init();
	mutex_unlock(&mcast_state_lock);
	if (ret)
		goto deinit;
	ret = mtk_npu_mcu_state_notifier_register(MCU_STATE_TYPE_NETSTOP,
						&mcast_stop_notifier);
	if (ret)
		goto deinit;
	ret = mtk_npu_mcu_state_notifier_register(MCU_STATE_TYPE_INIT,
						&mcast_init_notifier);
	if (ret)
		goto unregister_stop;
	ret = mtk_npu_mcu_state_notifier_register(MCU_STATE_TYPE_FREERUN,
						&mcast_freerun_notifier);
	if (ret)
		goto unregister_init;
	/* No callback is published yet. A reset during registration may have
	 * changed the firmware mapping; resolve it again before publishing.
	 */
	mutex_lock(&mcast_state_lock);
	ret = mtk_npu_mcu_alive() ? mtk_npu_mcast_restore() : -EBUSY;
	if (!ret)
		mcast_state = MCAST_UP;
	mutex_unlock(&mcast_state_lock);
	if (ret)
		goto unregister_freerun;
	ret = mtk_npu_mcast_sysfs_init();
	if (ret)
		goto unregister_freerun;
	ret = mtk_npu_mcast_procfs_init();
	if (ret)
		goto sysfs_deinit;
	ret = mtk_npu_mcast_publish();
	if (ret)
		goto procfs_deinit;
	mutex_unlock(&mcast_lifecycle);
	return 0;

procfs_deinit:
	mtk_npu_mcast_procfs_deinit();
sysfs_deinit:
	mtk_npu_mcast_sysfs_deinit();
unregister_freerun:
	mtk_npu_mcu_state_notifier_unregister(MCU_STATE_TYPE_FREERUN, &mcast_freerun_notifier);
unregister_init:
	mtk_npu_mcu_state_notifier_unregister(MCU_STATE_TYPE_INIT, &mcast_init_notifier);
unregister_stop:
	mtk_npu_mcu_state_notifier_unregister(MCU_STATE_TYPE_NETSTOP, &mcast_stop_notifier);
deinit:
	mutex_lock(&mcast_state_lock);
	mcast_state = MCAST_DOWN;
	mtk_npu_mcast_deinit();
	nmcast.dev = NULL;
	nmcast.base = NULL;
	mutex_unlock(&mcast_state_lock);
out:
	mutex_unlock(&mcast_lifecycle);
	return ret;
}

void mtk_npu_mcast_stop(void)
{
	mutex_lock(&mcast_lifecycle);
	mutex_lock(&mcast_state_lock);
	if (mcast_state == MCAST_DOWN) {
		mutex_unlock(&mcast_state_lock);
		mutex_unlock(&mcast_lifecycle);
		return;
	}
	mcast_state = MCAST_DOWN;
	mtk_npu_mcast_pause();
	mutex_unlock(&mcast_state_lock);
	/* Never wait for a notifier while holding its state lock. */
	mtk_npu_mcu_state_notifier_unregister(MCU_STATE_TYPE_FREERUN, &mcast_freerun_notifier);
	mtk_npu_mcu_state_notifier_unregister(MCU_STATE_TYPE_INIT, &mcast_init_notifier);
	mtk_npu_mcu_state_notifier_unregister(MCU_STATE_TYPE_NETSTOP, &mcast_stop_notifier);
	mtk_npu_mcast_unpublish();
	mtk_npu_mcast_procfs_deinit();
	mtk_npu_mcast_sysfs_deinit();
	mutex_lock(&mcast_state_lock);
	mtk_npu_mcast_deinit();
	nmcast.dev = NULL;
	nmcast.base = NULL;
	mutex_unlock(&mcast_state_lock);
	mutex_unlock(&mcast_lifecycle);
}
