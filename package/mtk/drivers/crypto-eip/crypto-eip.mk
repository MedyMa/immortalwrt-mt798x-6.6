# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (C) 2023 MediaTek Inc.
#
# Author: Chris.Chou <chris.chou@mediatek.com>
#         Ren-Ting Wang <ren-ting.wang@mediatek.com>

# Configure for crypto-eip top makefile
EIP_KERNEL_PKGS+= \
	crypto-eip \
	crypto-eip-inline \
	crypto-eip-autoload

ifneq ($(CONFIG_PACKAGE_kmod-crypto-eip),)
EXTRA_KCONFIG+= \
	CONFIG_CRYPTO_OFFLOAD_INLINE=$(CONFIG_CRYPTO_OFFLOAD_INLINE)
endif

ifeq ($(CONFIG_CRYPTO_OFFLOAD_INLINE),y)
EXTRA_KCONFIG+= \
	CONFIG_MTK_CRYPTO_EIP_INLINE=m \
	CONFIG_CRYPTO_XFRM_OFFLOAD_MTK_PCE=$(CONFIG_CRYPTO_XFRM_OFFLOAD_MTK_PCE) \
	CONFIG_MTK_TOPS_CAPWAP_DTLS=$(CONFIG_MTK_TOPS_CAPWAP_DTLS) \
	CONFIG_CRYPTO_MTK_DDK_LOOKASIDE=$(CONFIG_CRYPTO_MTK_DDK_LOOKASIDE) \
	CONFIG_CRYPTO_OFFLOAD_INLINE_FLOWBLOCK=$(CONFIG_CRYPTO_OFFLOAD_INLINE_FLOWBLOCK)

EXTRA_CFLAGS+= \
	-I$(LINUX_DIR)/drivers/net/ethernet/mediatek/ \
	-I$(KERNEL_BUILD_DIR)/pce/inc/
endif

ifeq ($(CONFIG_MTK_TOPS_CAPWAP_DTLS),y)
EXTRA_CFLAGS += \
	-DCONFIG_TOPS_TNL_NUM=$(CONFIG_TOPS_TNL_NUM)
endif

# crypto-eip kernel package configuration
define KernelPackage/crypto-eip
  CATEGORY:=MTK Properties
  SUBMENU:=Drivers
  TITLE:= EIP-197 Crypto Engine Driver
  DEFAULT:=n
  KCONFIG:= \
	CONFIG_CRYPTO_HW=y \
	CONFIG_CRYPTO_AUTHENC=y \
	CONFIG_CRYPTO_AES=y \
	CONFIG_CRYPTO_ECB=y \
	CONFIG_CRYPTO_AEAD=y \
	CONFIG_CRYPTO_DES=y \
	CONFIG_CRYPTO_MD5=y \
	CONFIG_CRYPTO_SHA1=y \
	CONFIG_CRYPTO_SHA256=y \
	CONFIG_CRYPTO_SHA512=y \
	CONFIG_CRYPTO_SHA3=y \
	CONFIG_CRYPTO_HMAC=y \
	CONFIG_INET_ESP=y \
	CONFIG_INET6_ESP=y \
	CONFIG_INET_ESP_OFFLOAD=y \
	CONFIG_INET6_ESP_OFFLOAD=y \
	CONFIG_XFRM_USER=y \
	CONFIG_XFRM_OFFLOAD=y
  CONFLICTS:=kmod-crypto-hw-safexcel
  DEPENDS:= \
	@TARGET_mediatek @!PACKAGE_kmod-crypto-hw-safexcel
  $(call AddDepends/crypto)
endef

define KernelPackage/crypto-eip/description
  Optional MT7988 EIP-197 inline IPsec ESP support with Linux XFRM policy
  checks. This port supports physical MTK GMAC devices and per-packet QDMA
  submission; it does not advertise crypto HNAT fast binding, PPPoE/VLAN,
  ESN, lookaside or flowblock offload. Enable only for a compatible SA.
endef

define KernelPackage/crypto-eip/config
	source "$(SOURCE)/Config.in"
endef

define KernelPackage/crypto-eip-inline
  CATEGORY:=MTK Properties
  SUBMENU:=Drivers
  TITLE:= EIP-197 Crypto Engine Inline Driver
  KCONFIG:=
  DEPENDS:= \
	@CRYPTO_OFFLOAD_INLINE \
	kmod-crypto-eip \
	kmod-crypto-eip-ddk \
	kmod-crypto-eip-ddk-ksupport \
	kmod-crypto-eip-ddk-ctrl \
	kmod-crypto-eip-ddk-ctrl-app \
	kmod-crypto-eip-ddk-engine \
	+kmod-pce \
	+kmod-mediatek_hnat
  FILES:=$(PKG_BUILD_DIR)/crypto-eip-inline.ko
  $(call AddDepends/crypto)
endef

define KernelPackage/crypto-eip-inline/description
  EIP197 inline mode. HW offload for IPsec ESP Tunnel mode.
endef

define KernelPackage/crypto-eip-autoload
  CATEGORY:=MTK Properties
  SUBMENU:=Drivers
  TITLE:= EIP-197 Crypto Engine Driver Autoload
  DEFAULT:=n
  AUTOLOAD:=$(call AutoLoad,60,crypto-eip-inline)
  KCONFIG:=
  DEPENDS:= kmod-crypto-eip-inline
endef

define KernelPackage/crypto-eip-autoload/description
  EIP197 Driver Autoload.
endef
