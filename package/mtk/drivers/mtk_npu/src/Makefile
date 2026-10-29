# SPDX-License-Identifier: GPL-2.0-or-later
#
# Copyright (c) 2025 MediaTek Inc. All Rights Reserved.
#
# Author: Ren-Ting Wang <ren-ting.wang@mediatek.com>
#

# Reject incompatible backends even when stale flags bypass package Kconfig.
npu-router-backends := $(CONFIG_MTK_NPU_HNAT) $(CONFIG_MTK_NPU_FLOW) $(CONFIG_MTK_NPU_MCAST)
npu-tunnel-protocols := $(CONFIG_MTK_NPU_GRE) $(CONFIG_MTK_NPU_GRETAP) \
	$(CONFIG_MTK_NPU_PPP) $(CONFIG_MTK_NPU_L2TP) $(CONFIG_MTK_NPU_L2TP_V2) \
	$(CONFIG_MTK_NPU_PPTP) $(CONFIG_MTK_NPU_VXLAN)

ifneq ($(filter y m,$(CONFIG_MTK_NPU_DPDK)),)
ifneq ($(filter y m,$(npu-router-backends) $(npu-tunnel-protocols) \
	$(CONFIG_MTK_NPU_ADPT_FWD) $(CONFIG_MTK_NPU_L4S) $(CONFIG_MTK_NPU_RATE_LIMIT)),)
$(error NPU DPDK requires other NPU packet backends and tunnel protocols disabled)
endif
endif

ifneq ($(filter y m,$(CONFIG_MTK_NPU_L4S)),)
ifneq ($(filter y m,$(npu-router-backends) $(npu-tunnel-protocols) \
	$(CONFIG_MTK_NPU_DPDK) $(CONFIG_MTK_NPU_ADPT_FWD) $(CONFIG_MTK_NPU_RATE_LIMIT)),)
$(error The dedicated NPU L4S firmware mode cannot share other packet backends)
endif
endif

ifneq ($(filter y m,$(CONFIG_MTK_NPU_FLOW)),)
ifneq ($(filter y m,$(CONFIG_MTK_NPU_HNAT) $(CONFIG_MTK_NPU_MCAST) \
	$(CONFIG_MTK_NPU_GRETAP) $(CONFIG_MTK_NPU_VXLAN) $(CONFIG_NET_MEDIATEK_HNAT)),)
$(error Native NPU FLOW requires vendor HNAT and its tunnel/multicast backends disabled)
endif
ifneq ($(CONFIG_NET_MEDIATEK_NPU_FLOW),y)
$(error Native NPU FLOW requires CONFIG_NET_MEDIATEK_NPU_FLOW=y in the kernel)
endif
endif

ifneq ($(filter y m,$(CONFIG_MTK_NPU_HNAT) $(CONFIG_MTK_NPU_MCAST)),)
ifeq ($(filter y m,$(CONFIG_NET_MEDIATEK_HNAT)),)
$(error NPU HNAT/multicast requires the vendor HNAT kernel provider)
endif
endif

ifneq ($(filter y m,$(CONFIG_MTK_NPU_PPTP) $(CONFIG_MTK_NPU_L2TP_V2)),)
ifneq ($(CONFIG_MTK_PPP_TUNNEL_INFO),y)
$(error NPU PPP tunnel protocols require CONFIG_MTK_PPP_TUNNEL_INFO=y in the kernel)
endif
endif

# The adapter borrows the core mapping and follows the same platform lifetime.
npu-$(CONFIG_MTK_NPU_DPDK) += npu-dpdk/init.o npu-dpdk/cdev.o npu-dpdk/mac-filter.o
ifneq ($(filter y m,$(CONFIG_MTK_NPU_DPDK)),)
npu-$(CONFIG_PROC_FS) += npu-dpdk/procfs.o
npu-$(CONFIG_SYSFS) += npu-dpdk/sysfs.o
endif
npu-$(CONFIG_MTK_NPU_FLOW) += npu-flow/init.o npu-flow/net-event.o npu-flow/tnl-offload.o
# The 24.10 HNAT adapter shares the core's platform/worker lifetime.
npu-$(CONFIG_MTK_NPU_HNAT) += npu-nf_hnat/init.o
npu-$(CONFIG_MTK_NPU_HNAT) += npu-nf_hnat/tnl-offload.o
npu-$(CONFIG_MTK_NPU_HNAT) += npu-nf_hnat/net-event.o
ifneq ($(filter y m,$(CONFIG_MTK_NPU_HNAT)),)
npu-$(CONFIG_MTK_NPU_STATISTIC) += npu-nf_hnat/statistic.o
endif
npu-$(CONFIG_MTK_NPU_MCAST) += npu-mcast/init.o npu-mcast/mcast.o
ifneq ($(filter y m,$(CONFIG_MTK_NPU_MCAST)),)
npu-$(CONFIG_PROC_FS) += npu-mcast/procfs.o
npu-$(CONFIG_SYSFS) += npu-mcast/sysfs.o
endif

obj-m += npu.o

ccflags-y += -I$(src)/inc
ccflags-y += -I$(src)/protocol/inc
ccflags-y += -I$(src)/npu-nf_hnat/inc
ccflags-y += -I$(src)/npu-flow/inc
ccflags-y += -I$(src)/npu-dpdk/inc
ccflags-y += -I$(src)/npu-mcast/inc

npu-y += firmware.o
npu-y += hwspinlock.o
npu-y += init.o
npu-y += mbox.o
npu-y += mcu.o
npu-y += misc.o
npu-y += netsys.o
npu-y += net-core.o
npu-y += net-event.o
npu-y += npu_params.o
npu-y += npu-ocd.o
npu-y += seq_gen.o
npu-y += ser.o
npu-y += tdma.o
npu-y += tnl_offload.o
npu-y += trm-fs.o
npu-y += trm.o
npu-y += wdt.o

npu-$(CONFIG_MTK_NPU_LOGGER) += logger.o
npu-$(CONFIG_MTK_NPU_MEM_TEST) += mem-test.o
npu-$(CONFIG_MTK_NPU_THERMAL) += thermal.o
npu-$(CONFIG_MTK_NPU_STATISTIC) += statistic.o

npu-y += protocol/mac/eth.o
npu-y += protocol/mac/ppp.o
npu-y += protocol/network/ip.o
npu-y += protocol/transport/udp.o
npu-$(CONFIG_MTK_NPU_GRETAP) += protocol/tunnel/gre/gretap.o
npu-$(CONFIG_MTK_NPU_L2TP_V2) += protocol/tunnel/l2tp/l2tpv2.o
npu-$(CONFIG_MTK_NPU_PPTP) += protocol/tunnel/pptp/pptp.o
npu-$(CONFIG_MTK_NPU_PPTP) += protocol/tunnel/pptp/pptp_seq.o
npu-$(CONFIG_MTK_NPU_VXLAN) += protocol/tunnel/vxlan/vxlan.o

npu-$(CONFIG_PROC_FS) += procfs.o
npu-$(CONFIG_SYSFS) += sysfs.o
npu-$(CONFIG_SYSFS) += sysfs-net.o

npu-$(CONFIG_DEBUG_FS) += debugfs.o
