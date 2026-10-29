# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2025 Mediatek Inc. All Rights Reserved.
# Author: Ren-Ting Wang <ren-ting.wang@mediatek.com>
#

NPU_KERNEL_PKGS+= \
	npu-mcast \
	npu-mcast-autoload

ifneq ($(filter y m,$(CONFIG_PACKAGE_kmod-npu-mcast)),)
EXTRA_KCONFIG+= \
	CONFIG_MTK_NPU_MCAST=$(CONFIG_MTK_NPU_MCAST)
endif # CONFIG_PACKAGE_kmod-npu-mcast

define KernelPackage/npu-mcast
  CATEGORY:=MTK Properties
  SUBMENU:=Drivers
  TITLE:=MediaTek Network Processor Unit with Multicast Feature
  DEFAULT:=y if MTK_NPU_24_10_MCAST_BACKEND
  KCONFIG:=
  DEPENDS:= \
	@MTK_NPU_24_10_MCAST_BACKEND \
	kmod-npu \
	+@MTK_NPU_MCAST
endef

define KernelPackage/npu-mcast/description
  Support for MediaTek Network processor Unit with IPv4/IPv6 multicast feature.
  The driver provides interfaces to add, delete, update multicast table and
  clients.
endef

define KernelPackage/npu-mcast-autoload
  CATEGORY:=MTK Properties
  SUBMENU:=Drivers
  TITLE:= MediaTek Network Processor Unit with Multicast Feature Auto Load
  DEFAULT:=y if MTK_NPU_24_10_MCAST_BACKEND
  KCONFIG:=
  DEPENDS:= \
	kmod-npu-mcast \
	+kmod-npu-autoload
endef

define KernelPackage/npu-mcast-autoload/description
  Support for MediaTek Network Processor Unit Kernel Driver with IPv4/IPv6
  multicast feature auto load on system boot process.
endef
