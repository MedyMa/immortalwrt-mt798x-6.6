include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=mt_wifi_osal
PKG_RELEASE:=8
PKG_BUILD_DIR:=$(KERNEL_BUILD_DIR)/$(PKG_NAME)/wifi_osal
PKG_SOURCE:=mt_wifi_osal-a53418.tar.xz
PKG_SOURCE_URL:=https://raw.githubusercontent.com/MedyMa/immortalwrt-mt798x-6.6/mt798x-mt799x-6.6-mtwifi/dl
PKG_HASH:=dfa4b178f198504a707dec957242f8ede1a732823e940faa6304c723ac3224ad
PKG_CONFIG_DEPENDS:=CONFIG_MTK_WIFI_OSAL_CFG80211_SUPPORT \
	CONFIG_MTK_WIFI_OSAL_DEBUG CONFIG_MTK_WIFI_OSAL_CUSTOM_VPRINTK \
	CONFIG_MTK_WIFI_OSAL_CUSTOM_VPRINTK_FACILITY CONFIG_MTK_WIFI_OSAL_NETIF_RX_DISPATCH CONFIG_MTK_WIFI_OSAL_FIPS_SUPPORT \
	CONFIG_MTK_WIFI7_VLAN_GTK_SUPPORT CONFIG_MTK_WIFI7_CCN67_SPLIT_MAC_SUPPORT

include $(INCLUDE_DIR)/package.mk

define KernelPackage/$(PKG_NAME)
  CATEGORY:=MTK Properties
  SUBMENU:=Drivers
  TITLE:=MediaTek WiFi OS Abstraction Layer
  DEFAULT:=n
  DEPENDS:=+MTK_WIFI_OSAL_CFG80211_SUPPORT:kmod-cfg80211
  CONFLICTS:=kmod-mt_wifi_cmn
  FILES:=$(PKG_BUILD_DIR)/mt_wifi_osal.ko
  AUTOLOAD:=$(call AutoLoad,60,$(if $(CONFIG_MTK_WIFI_OSAL_CFG80211_SUPPORT), cfg80211) mt_wifi_osal,1)
endef

BACKPORT_VERSION = $(sort $(patsubst backports-%,%,$(notdir \
	$(wildcard $(KERNEL_BUILD_DIR)/backports-* $(KERNEL_BUILD_DIR)/mac80211-regular/backports-*))))
EXTRA_KCONFIG += CFG_CFG80211_VERSION=$(if $(CONFIG_MTK_WIFI_OSAL_CFG80211_SUPPORT),$(firstword $(BACKPORT_VERSION)))

PKG_EXTRA_CFLAGS:= $(if $(CONFIG_MTK_WIFI_OSAL_CFG80211_SUPPORT), -DCFG80211_SUPPORT) \
		$(if $(CONFIG_MTK_WIFI_OSAL_DEBUG), -DMT_DEBUG) \
		$(if $(CONFIG_MTK_WIFI_OSAL_CUSTOM_VPRINTK), -DCUSTOM_FACILITY=$(CONFIG_MTK_WIFI_OSAL_CUSTOM_VPRINTK_FACILITY)) \
		$(if $(CONFIG_MTK_WIFI7_VLAN_GTK_SUPPORT), -DCONFIG_VLAN_GTK_SUPPORT) \
		$(if $(CONFIG_MTK_WIFI7_CCN67_SPLIT_MAC_SUPPORT), -DCCN67_SPLIT_MAC_SUPPORT) \
		$(if $(CONFIG_MTK_WIFI_OSAL_NETIF_RX_DISPATCH), -DCONFIG_NETIF_RX_DISPATCH) \
		$(if $(CONFIG_MTK_WIFI_OSAL_FIPS_SUPPORT), -DCONFIG_MTK_WIFI_OSAL_FIPS_SUPPORT)

PKG_EXTRA_CFLAGS += -Wno-error=missing-prototypes -Wno-empty-body

NOSTDINC_FLAGS :=
ifeq ($(CONFIG_MTK_WIFI_OSAL_CFG80211_SUPPORT),y)
NOSTDINC_FLAGS += \
        $(KERNEL_NOSTDINC_FLAGS) \
        -DBACKPORT_NOSTDINC \
        -I$(PKG_BUILD_DIR) \
        -I$(PKG_BUILD_DIR)/include \
        -I$(STAGING_DIR)/usr/include/mac80211-backport \
        -I$(STAGING_DIR)/usr/include/mac80211-backport/uapi \
        -I$(STAGING_DIR)/usr/include/mac80211 \
        -I$(STAGING_DIR)/usr/include/mac80211/uapi \
        -include backport/backport.h
endif

define KernelPackage/$(PKG_NAME)/config
	source "$(SOURCE)/Config.in"
endef

define Build/Compile
	$(if $(CONFIG_MTK_WIFI_OSAL_CFG80211_SUPPORT),\
		$(if $(filter 1,$(words $(BACKPORT_VERSION))),true,\
			$(error OSAL requires one matching mac80211 backport version; found $(BACKPORT_VERSION))))
	+$(KERNEL_MAKE) $(PKG_JOBS) \
		M="$(PKG_BUILD_DIR)" \
		NOSTDINC_FLAGS="$(NOSTDINC_FLAGS)" \
		EXTRA_CFLAGS="$(PKG_EXTRA_CFLAGS) $(EXTRA_CFLAGS)" \
		$(EXTRA_KCONFIG) \
		KBUILD_EXTRA_SYMBOLS="$(filter-out $(PKG_SYMVERS_DIR)/mt_wifi_cmn.symvers,$(wildcard $(PKG_SYMVERS_DIR)/*.symvers))" \
		modules
endef

define Build/InstallDev
	mkdir -p $(1)/usr/include/mt_wifi/os
	$(CP) $(PKG_BUILD_DIR)/inc/* $(1)/usr/include/mt_wifi/os
endef

define KernelPackage/$(PKG_NAME)/description
  MediaTek WiFi OS Abstraction Layer
endef


$(eval $(call KernelPackage,mt_wifi_osal))
