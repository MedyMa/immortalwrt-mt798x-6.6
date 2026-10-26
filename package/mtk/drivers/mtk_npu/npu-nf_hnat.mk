# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2025 Mediatek Inc. All Rights Reserved.
# Author: Ren-Ting Wang <ren-ting.wang@mediatek.com>
#

NPU_KERNEL_PKGS+= \
	npu-nf_hnat \
	npu-nf_hnat-autoload

ifneq ($(filter y m,$(CONFIG_PACKAGE_kmod-npu-nf_hnat)),)
EXTRA_KCONFIG+= \
	CONFIG_MTK_NPU_HNAT=$(CONFIG_MTK_NPU_HNAT)
endif # CONFIG_PACKAGE_kmod-npu-nf_hnat

define KernelPackage/npu-nf_hnat
  CATEGORY:=MTK Properties
  SUBMENU:=Drivers
  TITLE:=MediaTek Network Processor Unit with HNAT Kernel Driver
  KCONFIG:= CONFIG_MTK_PPP_TUNNEL_INFO=y
  DEPENDS:= \
	@MTK_NPU_24_10_TUNNEL_BACKEND \
	kmod-npu \
	kmod-mediatek_hnat \
	+@MTK_NPU_HNAT
endef

define KernelPackage/npu-nf_hnat/description
  Support for MediaTek Network Processor Unit with HNAT Kernel Driver.
  The driver controls the system to offload network packets to TOPS with the
  help of HNAT hardware.
endef

define KernelPackage/npu-nf_hnat-autoload
  CATEGORY:=MTK Properties
  SUBMENU:=Drivers
  TITLE:= MediaTek Network Processor Unit with HNAT Kernel Driver Auto Load
  KCONFIG:=
  DEPENDS:= \
	kmod-npu-nf_hnat \
	+kmod-npu-autoload
endef

define KernelPackage/npu-nf_hnat-autoload/description
  Support for MediaTek Network Processor Unit Kernel Driver with HNAT
  driver support auto load on system boot process.
endef
