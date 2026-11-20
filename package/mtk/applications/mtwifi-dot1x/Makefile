include $(TOPDIR)/rules.mk

PKG_NAME:=mtwifi-dot1x
PKG_VERSION:=1.0
PKG_RELEASE:=1
# 8021xd/src tree in hanwckf/immortalwrt-mt798x at 19e5d9d6.
PKG_SOURCE:=$(PKG_NAME)-c6d5459f.tar.xz
PKG_SOURCE_URL:=https://raw.githubusercontent.com/MedyMa/immortalwrt-mt798x-25.12/main/dl/
PKG_HASH:=d20d4865c7d60de658de2190c960e1246c726ae3f503ff0a9e6c98783b68b8ca
PKG_LICENSE:=GPL-2.0-only BSD-3-Clause
PKG_LICENSE_FILES:=COPYING NOTICE
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/mtwifi-dot1x
  SECTION:=net
  CATEGORY:=Network
  TITLE:=MediaTek WEXT 802.1X authenticator
  DEPENDS:=+libgcc
endef

MAKE_FLAGS+=CONFIG_SUPPORT_OPENWRT=y
TARGET_CFLAGS+=-DCONFIG_SUPPORT_OPENWRT -std=gnu17

define Package/mtwifi-dot1x/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/8021xd $(1)/usr/bin/
endef

$(eval $(call BuildPackage,mtwifi-dot1x))
