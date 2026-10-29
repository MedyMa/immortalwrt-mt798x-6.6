#
# This is free software, licensed under the GNU General Public License v2.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=mtwifi-cfg
PKG_LICENSE:=GPL-2.0+
PKG_VERSION:=1
PKG_RELEASE:=2

include $(INCLUDE_DIR)/package.mk

define Package/mtwifi-cfg
  SECTION:=MTK Properties
  CATEGORY:=MTK Properties
  SUBMENU:=Applications
  TITLE:=mtwifi configure scripts for mt798x
  DEPENDS:=+iwinfo +wifi-dats +wifi-scripts +lua-cjson +libuci-lua +luci-lib-nixio +datconf-lua @!PACKAGE_luci-app-mtk @!PACKAGE_wifi-profile
endef

define Build/Compile
endef

define Package/mtwifi-cfg/install
	$(INSTALL_DIR) $(1)/sbin $(1)/usr/sbin $(1)/usr/lib/lua
	$(INSTALL_DIR) $(1)/lib/netifd/wireless/ $(1)/lib/wifi/

	$(INSTALL_DIR) $(1)/etc/hotplug.d/net/ $(1)/etc/init.d $(1)/lib/upgrade/keep.d

	$(INSTALL_BIN) ./files/netifd/mtwifi.sh $(1)/lib/netifd/wireless/
	$(INSTALL_BIN) ./files/mtwifi.sh $(1)/lib/wifi/
	$(INSTALL_BIN) ./files/mtwifi-cfg/mtwifi_cfg $(1)/sbin/
	$(INSTALL_BIN) ./files/mtwifi-cfg/mtwifi_mlo $(1)/usr/sbin/mtwifi-mlo-status
	$(INSTALL_BIN) ./files/mtwifi-cfg/mtwifi_mlo $(1)/usr/sbin/mtwifi-mlo-control
	$(INSTALL_BIN) ./files/mtwifi-cfg/mtwifi-mlo-recover $(1)/etc/init.d/mtwifi-mlo-recover
	$(INSTALL_DATA) ./files/mtwifi-cfg/keep.d $(1)/lib/upgrade/keep.d/mtwifi-cfg
	$(INSTALL_BIN) ./files/mtwifi-cfg/inspect.lua $(1)/usr/lib/lua/
	$(INSTALL_BIN) ./files/mtwifi-cfg/mtwifi_defs.lua $(1)/usr/lib/lua/
	$(INSTALL_DATA) ./files/mtwifi-cfg/mtwifi_bandwidth.lua $(1)/usr/lib/lua/
	$(INSTALL_BIN) ./files/mtwifi-cfg/mtwifi_utils.lua $(1)/usr/lib/lua/
	$(INSTALL_BIN) ./files/l1util/l1util $(1)/sbin/
	$(LN) l1util $(1)/sbin/l1dat
	$(INSTALL_BIN) ./files/l1util/l1dat_parser.lua $(1)/usr/lib/lua/
	$(INSTALL_DATA) ./files/hotplug/10-mtwifi-detect $(1)/etc/hotplug.d/net/
endef

$(eval $(call BuildPackage,mtwifi-cfg))
