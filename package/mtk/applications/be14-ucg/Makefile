include $(TOPDIR)/rules.mk

PKG_NAME:=be14-ucg
PKG_VERSION:=1.0
PKG_RELEASE:=13
PKG_LICENSE:=GPL-2.0-or-later

include $(INCLUDE_DIR)/package.mk

define Package/be14-ucg
  SECTION:=net
  CATEGORY:=Network
  TITLE:=UniFi inform client for the BPI-R4 BE14
  DEPENDS:=+ip-full +ip-bridge +kmod-macvlan +unshare +mtwifi-cfg +libubus-lua +rpcd +rpcd-mod-iwinfo +rpcd-mod-luci +umdns +python3-cryptography +python3-urllib +python3-openssl +python3-codecs
  PKGARCH:=all
endef

define Package/be14-ucg/conffiles
/etc/config/be14_ucg
endef

define Build/Compile
endef

define Package/be14-ucg/install
	$(INSTALL_DIR) $(1)/usr/lib/be14-ucg $(1)/etc/config $(1)/etc/init.d $(1)/usr/bin $(1)/lib/upgrade/keep.d
	$(INSTALL_DATA) ./files/*.py $(1)/usr/lib/be14-ucg/
	chmod 0755 $(1)/usr/lib/be14-ucg/wired_host.py
	$(INSTALL_BIN) ./files/wireless.lua $(1)/usr/lib/be14-ucg/
	$(INSTALL_BIN) ./files/syswrapper.sh $(1)/usr/bin/
	$(INSTALL_BIN) ./files/be14-ucg-status $(1)/usr/bin/
	$(INSTALL_CONF) ./files/config $(1)/etc/config/be14_ucg
	$(INSTALL_BIN) ./files/init $(1)/etc/init.d/be14-ucg
	$(INSTALL_DATA) ./files/keep $(1)/lib/upgrade/keep.d/be14-ucg
endef

$(eval $(call BuildPackage,be14-ucg))
