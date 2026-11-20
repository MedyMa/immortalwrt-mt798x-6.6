#!/bin/sh
#
# Copyright (c) 2023, hanwckf <hanwckf@vip.qq.com>
#

. /lib/netifd/netifd-wireless.sh

init_wireless_driver "$@"

LOCK_FILE="/tmp/mtwifi.lock"

MTWIFI_MAX_AP_IDX=15
MTWIFI_MAX_APCLI_IDX=0
MTWIFI_CFG_IFNAME_KEY="mtwifi_ifname"

drv_mtwifi_init_device_config() {
	config_add_int txpower beacon_int channel2
	config_add_boolean mu_beamformer dbdc_main whnat vht_1024 acs_exclude_dfs sre_enable
	config_add_string country twt eht_restricted_twt tx_burst doth tx_preamble he_ldpc dfs zw_dfs bss_color
	config_add_array chanlist acs_skiplist
}

drv_mtwifi_init_iface_config() {
	config_add_string 'ssid:string' macfilter macfile bssid kicklow assocthres 'macaddr:macaddr'
	config_add_string auth_server auth_secret ownip
	config_add_int auth_port
	config_add_boolean mlo wmm hidden isolate ieee80211k bss_transition mac_repeater
	config_add_int mldgroup
	config_add_int wpa_group_rekey frag rts dtim_period ieee80211w
	config_add_array 'maclist:list(macaddr)'
	config_add_boolean mumimo_dl mumimo_ul ofdma_dl ofdma_ul amsdu autoba uapsd
}

# These WEXT handlers have no driver-specific VLAN or station attributes.
drv_mtwifi_init_vlan_config() { :; }
drv_mtwifi_init_station_config() { :; }

drv_mtwifi_cleanup() {
	return
}

mtwifi_notify() {
	local previous result
	json_set_namespace wdev previous
	"$@"
	result=$?
	json_set_namespace "$previous"
	return "$result"
}

mtwifi_vif_config() {
	local name="$1"
	local ifname=""
	local disabled=""

	json_select config
	json_get_var disabled disabled
	json_select ..

	[ "$disabled" = "1" ] && return

	json_get_var ifname $MTWIFI_CFG_IFNAME_KEY

	if [ -n "$ifname" ]; then
		logger -t "netifd-mtwifi" "add $ifname to vifidx $name"
		mtwifi_notify _wireless_add_vif "$name" "$ifname" || MTWIFI_NOTIFY_FAILED=1
	fi
}

mtwifi_vif_ap_set_data() {
	local ifname=""

	if [ ! $AP_IDX -gt $MTWIFI_MAX_AP_IDX ]; then
		ifname="${MTWIFI_AP_IF_PREFIX}${AP_IDX}"
		AP_IDX=$((AP_IDX+1))
	fi

	json_add_string "$MTWIFI_CFG_IFNAME_KEY" "$ifname"
}

mtwifi_vif_sta_set_data() {
	local ifname=""

	if [ ! $APCLI_IDX -gt $MTWIFI_MAX_APCLI_IDX ]; then
		ifname="${MTWIFI_APCLI_IF_PREFIX}${APCLI_IDX}"
		APCLI_IDX=$((APCLI_IDX+1))
	fi

	json_add_string "$MTWIFI_CFG_IFNAME_KEY" "$ifname"
}

drv_mtwifi_setup() {
	local dev="$1" main_ifname wait_file

	json_add_string device "$dev"

	if ! lock "$LOCK_FILE"; then
		wireless_setup_failed "MTWIFI_LOCK_FAILED"
		return 1
	fi

	main_ifname="$(l1util get "$dev" main_ifname)"
	case "$(l1util get "$dev" INDEX):$main_ifname" in
		MT7990:ra0|MT7990:rai0|MT7990:rax0)
			wait_file="/var/run/mtwifi-wait-$main_ifname"
			if [ ! -d "/sys/class/net/$main_ifname" ]; then
				printf '%s\n' "$dev" > "$wait_file"
				# The add event may have arrived while publishing the waiter.
				if [ ! -d "/sys/class/net/$main_ifname" ]; then
					wireless_setup_failed "MTWIFI_WAITING_FOR_NETDEVICE"
					lock -u "$LOCK_FILE"
					return 1
				fi
			fi
			rm -f "$wait_file"
			;;
	esac

	logger -t "netifd-mtwifi" "up: $dev"

	MTWIFI_AP_IF_PREFIX="$(l1util get "$dev" ext_ifname)"
	MTWIFI_APCLI_IF_PREFIX="$(l1util get "$dev" apcli_ifname)"

	AP_IDX=0
	for_each_interface ap mtwifi_vif_ap_set_data

	APCLI_IDX=0
	for_each_interface sta mtwifi_vif_sta_set_data

	if ! json_dump | /sbin/mtwifi_cfg setup; then
		wireless_setup_failed "MTWIFI_CFG_SETUP_FAILED"
		lock -u "$LOCK_FILE"
		return 1
	fi

	MTWIFI_NOTIFY_FAILED=0
	local dot1x_pid
	if [ -r "/var/run/8021xd_${main_ifname}.pid" ]; then
		read -r dot1x_pid < "/var/run/8021xd_${main_ifname}.pid"
		mtwifi_notify _wireless_add_process "$dot1x_pid" /usr/bin/8021xd 1 || MTWIFI_NOTIFY_FAILED=1
	fi
	for_each_interface "ap sta" mtwifi_vif_config

	if [ "$MTWIFI_NOTIFY_FAILED" = 1 ] || ! mtwifi_notify _wireless_set_up; then
		/sbin/mtwifi_cfg down "$dev"
		wireless_setup_failed "MTWIFI_NETIFD_NOTIFY_FAILED"
		lock -u "$LOCK_FILE"
		return 1
	fi
	lock -u "$LOCK_FILE"
}

drv_mtwifi_teardown() {
	local dev="$1"

	lock "$LOCK_FILE" || return 1

	logger -t "netifd-mtwifi" "down: $dev"

	local result=0
	/sbin/mtwifi_cfg down "$dev" || result=1
	# A disabled primary radio can still change the chip's WHNAT setting.
	/sbin/mtwifi_cfg whnat || logger -t netifd-mtwifi "WHNAT update failed"

	lock -u "$LOCK_FILE"
	return "$result"
}

add_driver mtwifi
