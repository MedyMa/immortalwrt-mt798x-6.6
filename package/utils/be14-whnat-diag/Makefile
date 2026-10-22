include $(TOPDIR)/rules.mk

PKG_NAME:=be14-whnat-diag
PKG_VERSION:=1
PKG_RELEASE:=2
PKG_LICENSE:=GPL-2.0-only
PKGARCH:=all

include $(INCLUDE_DIR)/package.mk

define Package/be14-whnat-diag
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=BE14 WHNAT diagnostic capture
  DEPENDS:=@TARGET_mediatek_filogic_a73 +kmod-mediatek_hnat +kmod-mt_wifi7 +kmod-mtk_pci
endef

define Package/be14-whnat-diag/description
  Capture software diagnostic counters for matching BE14 WHNAT kernel modules.
  Diagnostics stay disabled until explicitly requested by the operator.
endef

define Build/Compile
	sh -n $(CURDIR)/files/collect-be14-whnat-window
endef

define Package/be14-whnat-diag/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) ./files/collect-be14-whnat-window $(1)/usr/sbin/collect-be14-whnat-window
endef

$(eval $(call BuildPackage,be14-whnat-diag))
