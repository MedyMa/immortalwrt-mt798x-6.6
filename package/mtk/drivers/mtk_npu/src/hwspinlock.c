// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2025 MediaTek Inc. All Rights Reserved.
 *
 * Author: Ren-Ting Wang <ren-ting.wang@mediatek.com>
 */

#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/err.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/platform_device.h>

#include "npu/hwspinlock.h"
#include "npu/internal.h"
#include "npu/npu.h"

#define SEMA_ID				(BIT(CORE_AP))
#define HWSPINLOCK_TIMEOUT_US		1000

static inline u32 hwspinlock_read(u32 reg)
{
	return readl(npu.base + reg);
}

static inline void hwspinlock_write(u32 reg, u32 val)
{
	writel(val, npu.base + reg);
}

static inline u32 __mtk_npu_hwspinlock_get_reg(enum hwspinlock_group grp, u32 slot)
{
	if (unlikely(slot >= HWSPINLOCK_SLOT_MAX ||
		     (unsigned int)grp >= __HWSPINLOCK_GROUP_MAX))
		return 0;

	if (grp == HWSPINLOCK_GROUP_TOP)
		return HWSPINLOCK_TOP_BASE + slot * 4;
	else
		return HWSPINLOCK_CLUST_BASE + slot * 4;
}

/*
 * try take NPU HW spinlock
 * return 1 on success
 * return 0 on failure
 */
int mtk_npu_hwspin_try_lock(enum hwspinlock_group grp, u32 slot)
{
	u32 reg = __mtk_npu_hwspinlock_get_reg(grp, slot);

	if (!reg || !npu.base || reg + sizeof(u32) > npu.size)
		return 0;

	hwspinlock_write(reg, SEMA_ID);

	return hwspinlock_read(reg) == SEMA_ID ? 1 : 0;
}

int mtk_npu_hwspin_lock(enum hwspinlock_group grp, u32 slot)
{
	u32 reg = __mtk_npu_hwspinlock_get_reg(grp, slot);
	int taken;

	if (!reg || !npu.base || reg + sizeof(u32) > npu.size)
		return -EINVAL;

	/* Firmware can stop while owning a semaphore. Never spin forever with
	 * local IRQs disabled, and never let the caller enter without ownership.
	 */
	return read_poll_timeout_atomic(mtk_npu_hwspin_try_lock, taken, taken,
				       1, HWSPINLOCK_TIMEOUT_US, false, grp, slot);
}

void mtk_npu_hwspin_unlock(enum hwspinlock_group grp, u32 slot)
{
	u32 reg = __mtk_npu_hwspinlock_get_reg(grp, slot);

	if (!reg || !npu.base || reg + sizeof(u32) > npu.size)
		return;

	if (hwspinlock_read(reg) == SEMA_ID)
		hwspinlock_write(reg, SEMA_ID);
}
