#!/bin/sh
#
# Copyright (C) 2023, hanwckf <hanwckf@vip.qq.com>
#

append DRIVERS "mtwifi"

detect_mtwifi() {
	local dev dev_list idx ifname
	local band htmode htbsscoex ssid dbdc_main channel encryption
	if [ -d "/sys/module/mt_wifi" ]; then
		dev_list="$(l1util list)"
		for dev in $dev_list; do
			encryption="none"
			channel="auto"
			# Newer wifi-scripts do not preload the UCI shell cache.
			# Only initialize missing devices; retain every existing section.
			uci -q get "wireless.${dev}" >/dev/null || {
				ifname="$(l1util get "$dev" main_ifname)"
				# The MT7990 profile lists all three bands as soon as the module loads.
				# Publish a new radio only after its netdevice can accept setup.
				case "$dev:$ifname" in
					MT7990_*:ra0|MT7990_*:rai0|MT7990_*:rax0)
						[ -d "/sys/class/net/$ifname" ] || continue ;;
				esac

				idx="$(l1util get ${dev} subidx)"
				[ "$idx" = "1" ] && dbdc_main="1" || dbdc_main="0"

				band="$(l1util get ${dev} band)"
				if [ -z "$band" ] || [ "$band" = "nil" ]; then
					[ "$idx" = "1" ] && band="2g" || band="5g"
				fi

				if [ "$band" = "2g" ]; then
					htmode="EHT40"
					htbsscoex="1"
					ssid="ImmortalWrt-2.4G"
					channel="auto"
				elif [ "$band" = "5g" ]; then
					htmode="EHT160"
					htbsscoex="0"
					ssid="ImmortalWrt-5G"
					channel="36"
				elif [ "$band" = "6g" ]; then
					htmode="EHT160"
					htbsscoex="0"
					ssid="ImmortalWrt-6G"
					channel="5"
					encryption="owe"
				fi

				uci -q batch <<-EOF
					set wireless.${dev}=wifi-device
					set wireless.${dev}.type=mtwifi
					set wireless.${dev}.phy=${ifname}
					set wireless.${dev}.band=${band}
					set wireless.${dev}.dbdc_main=${dbdc_main}
					set wireless.${dev}.channel=${channel}
					set wireless.${dev}.txpower=100
					set wireless.${dev}.htmode=${htmode}
					set wireless.${dev}.country=US
					set wireless.${dev}.mu_beamformer=1
					set wireless.${dev}.noscan=${htbsscoex}
					set wireless.${dev}.serialize=1

					set wireless.default_${dev}=wifi-iface
					set wireless.default_${dev}.device=${dev}
					set wireless.default_${dev}.network=lan
					set wireless.default_${dev}.mode=ap
					set wireless.default_${dev}.ssid=${ssid}
					set wireless.default_${dev}.encryption=${encryption}
EOF
				uci -q commit wireless
			} || return 1
			if [ "$(uci -q get "wireless.${dev}.type")" = mtwifi ]; then
				ifname="$(l1util get "$dev" main_ifname)"
				if [ -n "$ifname" ] && [ "$(uci -q get "wireless.${dev}.phy")" != "$ifname" ]; then
					uci -q set "wireless.${dev}.phy=$ifname" || return 1
					uci -q commit wireless || return 1
				fi
			fi
		done
	fi
}
