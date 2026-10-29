# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2026 Mediatek Inc. All Rights Reserved.
# Author: Ren-Ting Wang <ren-ting.wang@mediatek.com>
#

NPU_KERNEL_PKGS+= \
	npu-dpdk \
	npu-dpdk-autoload

ifneq ($(filter y m,$(CONFIG_PACKAGE_kmod-npu-dpdk)),)
EXTRA_KCONFIG+= \
	CONFIG_MTK_NPU_DPDK=$(CONFIG_MTK_NPU_DPDK)

EXTRA_CFLAGS+= \
	-DCONFIG_MTK_NPU_DPDK_MAC_FILTER_NUM=$(CONFIG_MTK_NPU_DPDK_MAC_FILTER_NUM)
endif # CONFIG_PACKAGE_kmod-npu-dpdk

define KernelPackage/npu-dpdk
  CATEGORY:=MTK Properties
  SUBMENU:=Drivers
  TITLE:=MediaTek Network Processor Unit with DPDK Feature
  DEFAULT:=y if MTK_NPU_24_10_DPDK_BACKEND
  KCONFIG:=
  DEPENDS:= \
	@MTK_NPU_24_10_DPDK_BACKEND \
	kmod-npu \
	+@MTK_NPU_DPDK
endef

define KernelPackage/npu-dpdk/description
  Support for MediaTek Network processor Unit with customized DPDK functions.
  The NPU driver supports to offload DPDK's MAC filter function.
endef

define KernelPackage/npu-dpdk-autoload
  CATEGORY:=MTK Properties
  SUBMENU:=Drivers
  TITLE:= MediaTek Network Processor Unit with DPDK Feature Auto Load
  KCONFIG:=
  DEPENDS:= \
	kmod-npu-dpdk \
	+kmod-npu-autoload
endef

define KernelPackage/npu-dpdk-autoload/description
  Support for MediaTek Network Processor Unit Kernel Driver with DPDK functions
  auto load on system boot process.
endef
