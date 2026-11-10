# All rights reserved.
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=mt_wifi_cmn
PKG_RELEASE:=2
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)
PKG_VERSION:=TEST
PKG_FILE_DEPENDS:=$(CURDIR)/src/
PKG_CONFIG_DEPENDS:=CONFIG_MTK_WIFI7_CFG80211_SUPPORT CONFIG_MTK_WIFI7_OSAL_SUPPORT
include $(INCLUDE_DIR)/package.mk

define KernelPackage/mt_wifi_cmn
  CATEGORY:=MTK Properties
  TITLE:=MTK wifi common layer
  FILES:=$(PKG_BUILD_DIR)/mt_wifi_cmn.ko
  SUBMENU:=Drivers
  MENU:=1
  AUTOLOAD:=$(call AutoLoad,60,$(if $(CONFIG_MTK_WIFI7_CFG80211_SUPPORT), cfg80211) mt_wifi_cmn)
  DEPENDS:=+MTK_WIFI7_CFG80211_SUPPORT:kmod-cfg80211
endef

NOSTDINC_FLAGS :=
ifeq ($(CONFIG_MTK_WIFI7_CFG80211_SUPPORT),y)
NOSTDINC_FLAGS += \
	$(KERNEL_NOSTDINC_FLAGS) \
	-DRT_CFG80211_SUPPORT \
	-DBACKPORT_NOSTDINC \
	-I$(PKG_BUILD_DIR) \
	-I$(STAGING_DIR)/usr/include/mac80211-backport \
	-I$(STAGING_DIR)/usr/include/mac80211-backport/uapi \
	-I$(STAGING_DIR)/usr/include/mac80211 \
	-I$(STAGING_DIR)/usr/include/mac80211/uapi \
	-include backport/backport.h
endif
NOSTDINC_FLAGS += -DMT_WIFI_COMMON_SUPPORT

define Build/Compile
	+$(MAKE) $(PKG_JOBS) -C "$(LINUX_DIR)" \
		$(KERNEL_MAKE_FLAGS) \
		M="$(PKG_BUILD_DIR)" \
		EXTRA_CFLAGS="$(EXTRA_CFLAGS)" \
		NOSTDINC_FLAGS="$(NOSTDINC_FLAGS)" \
		KBUILD_EXTRA_SYMBOLS="$(filter-out $(PKG_SYMVERS_DIR)/mt_wifi_osal.symvers,$(wildcard $(PKG_SYMVERS_DIR)/*.symvers))" \
		modules
endef

$(eval $(call KernelPackage,mt_wifi_cmn))
