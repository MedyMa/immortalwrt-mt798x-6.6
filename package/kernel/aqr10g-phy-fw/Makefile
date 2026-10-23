include $(TOPDIR)/rules.mk

PKG_NAME:=aqr10g-phy-fw
PKG_RELEASE:=1
PKG_LICENSE:=LicenseRef-Redistributable
PKG_MAINTAINER:=BananaPi-R4 community

include $(INCLUDE_DIR)/package.mk

AQR113C_FW:=Rhe-05.06-Candidate9-AQR_Mediatek_23B_P5_ID45824_LCLVER1.cld
AQR113C_URL:=https://raw.githubusercontent.com/shiyu1314/immortalwrt-mt798x-6.12/25.12-dev/package/kernel/aqr10g-phy-fw/files/
AQR113C_HASH:=19d73393d575fbe4018c1685fbdea2ae6fb59be3c995f608920a417c7e3f8d1c

AQR_G4_FW:=AQR-G4_v5.7.0-AQR_EVB_Generic_X3410_StdCfg_MDISwap_USX_ID46316_VER2148.cld
AQR_G4_URL:=https://raw.githubusercontent.com/shiyu1314/immortalwrt-mt798x-6.12/25.12-dev/package/kernel/aqr10g-phy-fw/files/
AQR_G4_HASH:=1c9a67faffe50da1a0efa374ec084a956b6ec64ca9d97d3ad6b1a8708d490a44

define Download/aqr113c-firmware
  FILE:=$(AQR113C_FW)
  URL:=$(AQR113C_URL)
  HASH:=$(AQR113C_HASH)
endef
$(eval $(call Download,aqr113c-firmware))

define Download/aqr-g4-firmware
  FILE:=$(AQR_G4_FW)
  URL:=$(AQR_G4_URL)
  HASH:=$(AQR_G4_HASH)
endef
$(eval $(call Download,aqr-g4-firmware))

define Package/aqr10g-phy-fw
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=Aquantia AQR113C/CUX3410 10G PHY firmware
endef

define Package/aqr10g-phy-fw/description
  Firmware blobs referenced by the MT7988 AQR113C and CUX3410 DTS
  overlays. They allow the Aquantia driver to reload firmware after a
  software reset, which is needed for reliable warm reboot recovery.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Compile
endef

define Package/aqr10g-phy-fw/install
	$(INSTALL_DIR) $(1)/lib/firmware
	$(INSTALL_DATA) $(DL_DIR)/$(AQR113C_FW) $(1)/lib/firmware/
	$(INSTALL_DATA) $(DL_DIR)/$(AQR_G4_FW) $(1)/lib/firmware/
endef

$(eval $(call BuildPackage,aqr10g-phy-fw))
