#!/usr/bin/lua
--[[
 *
 * Copyright (C) 2023 hanwckf <hanwckf@vip.qq.com>
 *
 * 	This program is free software; you can redistribute it and/or modify
 * 	it under the terms of the GNU General Public License as published by
 * 	The Free Software Foundation; either version 2 of the License, or
 * 	(at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
]]

local mtwifi_defs = {}

mtwifi_defs.max_mbssid = 16
mtwifi_defs.max_acl_entry = 129

mtwifi_defs.vif_cfgs = {
    -- dat cfg = default val
    ["AuthMode"] = "OPEN",
    ["EncrypType"] = "NONE",
    ["PMFMFPC"] = "0",
    ["PMFMFPR"] = "0",
    ["PMFSHA256"] = "0",
    ["RekeyInterval"] = "3600",
    ["DefaultKeyID"] = "1",
    ["IEEE8021X"] = "0",
    ["Key1Type"] = "0",
    ["Key2Type"] = "0",
    ["Key3Type"] = "0",
    ["Key4Type"] = "0",
    ["PMKCachePeriod"] = "10",
    ["PreAuth"] = "0",
    ["RADIUS_Port"] = "1812",
    ["RADIUS_Server"] = "0",
    ["RekeyMethod"] = "DISABLE",
    ["session_timeout_interval"] = "0",
    ["Wapiifname"] = "",
    ["HideSSID"] = "0",
    ["WirelessMode"] = "",
    ["NoForwarding"] = "0",
    ["APSDCapable"] = "1",
    ["WmmCapable"] = "1",
    ["FragThreshold"] = "2346",
    ["RTSThreshold"] = "2347",
    ["HT_AMSDU"] = "1",
    ["HT_AutoBA"] = "1",
    ["HT_BAWinSize"] = "1024",
    ["HT_GI"] = "1",
    ["HT_LDPC"] = "1",
    ["HT_OpMode"] = "0",
    ["HT_PROTECT"] = "1",
    ["HT_STBC"] = "1",
    ["IgmpSnEnable"] = "0",
    ["RRMEnable"] = "1",
    ["WNMEnable"] = "0",
    ["VHT_BW_SIGNAL"] = "0",
    ["VHT_LDPC"] = "1",
    ["VHT_SGI"] = "1",
    ["VHT_STBC"] = "1",
    ["MuMimoDlEnable"] = "0",
    ["MuMimoUlEnable"] = "0",
    ["MuOfdmaDlEnable"] = "1",
    ["MuOfdmaUlEnable"] = "1",
    ["PpMuMimoDlEnable"] = "0",
    ["PpMuMimoUlEnable"] = "0",
    ["PpOfdmaDlEnable"] = "1",
    ["PpOfdmaUlEnable"] = "1",
    ["DLSCapable"] = "0",
    ["WdsEnable"] = "0",
    ["WscConfMode"] = "0",
    ["TxRate"] = "0",
    ["DtimPeriod"] = "1",
}

mtwifi_defs.vif_cfgs_idx = {
    ["WPAPSK"] = "12345678",
    ["SSID"] = "",
    ["RADIUS_Key"] = "",
}

mtwifi_defs.vif_mac = {
    ["MacAddress"] = "",
    -- ["MldAddr"] = "",
}

mtwifi_defs.vif_acl = {
    ["AccessPolicy"] = "0",
    ["AccessControlList"] = "",
}

mtwifi_defs.chip_cfgs = {
    -- uci config = dat config, default value
    ["beacon_int"] = { "BeaconPeriod" , "100"},
    ["whnat"] = { "WHNAT", "1"},
}

mtwifi_defs.reinstall_cfgs = {
    "BssidNum", "WHNAT", "E2pAccessMode",
    "HT_RxStream", "HT_TxStream", "WdsEnable"
}

mtwifi_defs.iwpriv_ap_cfgs = {
    -- uci config = iwpriv set cmd , default value
    ["kicklow"] = {"KickStaRssiLow", "0"},
    ["assocthres"] = {"AssocReqRssiThres" , "0"},
}

mtwifi_defs.enc2dat = {
    -- enc = AuthMode, EncrypType
    ["none"] = {"OPEN", "NONE"},
    ["sae"] = {"WPA3PSK", "AES"},
    ["sae+ccmp"] = {"WPA3PSK", "AES"},
    ["sae+gcmp"] = {"WPA3PSK", "GCMP128"},
    ["sae+ccmp256"] = {"WPA3PSK", "CCMP256"},
    ["sae+gcmp256"] = {"WPA3PSK", "GCMP256"},
    ["sae-mixed"] = {"WPA2PSKWPA3PSK", "AES"},
    ["sae-mixed+ccmp"] = {"WPA2PSKWPA3PSK", "AES"},
    ["psk2+tkip+ccmp"] = {"WPA2PSK", "TKIPAES"},
    ["psk2+tkip+aes"] = {"WPA2PSK", "TKIPAES"},
    ["psk2+tkip"] = {"WPA2PSK", "TKIP"},
    ["psk2+ccmp"] = {"WPA2PSK", "AES"},
    ["psk2+aes"] = {"WPA2PSK", "AES"},
    ["psk2"] = {"WPA2PSK", "AES"},
    ["psk+tkip+ccmp"] = {"WPAPSK", "AES"},
    ["psk+tkip+aes"] = {"WPAPSK", "AES"},
    ["psk+tkip"] = {"WPAPSK", "AES"},
    ["psk+ccmp"] = {"WPAPSK", "AES"},
    ["psk+aes"] = {"WPAPSK", "AES"},
    ["psk"] = {"WPAPSK", "AES"},
    ["psk-mixed+tkip+ccmp"] = {"WPAPSKWPA2PSK", "TKIPAES"},
    ["psk-mixed+tkip+aes"] = {"WPAPSKWPA2PSK", "TKIPAES"},
    ["psk-mixed+tkip"] = {"WPAPSKWPA2PSK", "TKIP"},
    ["psk-mixed+ccmp"] = {"WPAPSKWPA2PSK", "AES"},
    ["psk-mixed+aes"] = {"WPAPSKWPA2PSK", "AES"},
    ["psk-mixed"] = {"WPAPSKWPA2PSK", "AES"},
    ["wpa2"] = {"WPA2", "AES"},
    ["wpa2+ccmp"] = {"WPA2", "AES"},
    ["wpa2+aes"] = {"WPA2", "AES"},
    ["owe"] = {"OWE", "AES"},
    ["owe+ccmp"] = {"OWE", "AES"},
}

function mtwifi_defs.disabled(value)
    return value == true or value == 1 or value == "1"
end

local function radius_ipv4(value)
    if type(value) ~= "string" then return false end
    local a, b, c, d = value:match("^(%d+)%.(%d+)%.(%d+)%.(%d+)$")
    return a and tonumber(a) > 0 and tonumber(a) < 224 and tonumber(b) < 256 and
        tonumber(c) < 256 and tonumber(d) < 256 and
        value == string.format("%d.%d.%d.%d", a,b,c,d)
end

function mtwifi_defs.security(config)
    local encryption = config.encryption or "none"
    local enc = mtwifi_defs.enc2dat[encryption]
    if not enc then return nil, "UNSUPPORTED_ENCRYPTION" end

    local auth, cipher = enc[1], enc[2]
    local pmf = tonumber(config.ieee80211w or "")
    if config.ieee80211w ~= nil and config.ieee80211w ~= "" and
        (not pmf or pmf < 0 or pmf > 2 or pmf % 1 ~= 0) then
        return nil, "INVALID_PMF"
    end
    if auth == "WPA3PSK" or auth == "OWE" then
        if pmf and pmf ~= 2 then return nil, "WPA3_REQUIRES_PMF" end
        pmf = 2
    elseif auth == "WPA2PSKWPA3PSK" then
        if pmf == 0 then return nil, "SAE_TRANSITION_REQUIRES_PMF" end
        pmf = pmf or 1
    elseif auth ~= "WPA2" and auth ~= "WPA2PSK" and auth ~= "WPAPSKWPA2PSK" then
        pmf = 0
    end
    if cipher == "TKIP" or cipher == "TKIPAES" then
        if pmf and pmf > 0 then return nil, "PMF_REQUIRES_CCMP" end
    end

    local key = ""
    if auth == "WPA2" then
        local server = config.auth_server
        if type(server) == "table" and #server == 1 then server = server[1] end
        local port, secret = tonumber(config.auth_port or 1812), config.auth_secret
        if config.mode ~= "ap" or not radius_ipv4(server) or not port or port % 1 ~= 0 or
            port < 1 or port > 65535 or type(secret) ~= "string" or #secret < 1 or
            #secret > 63 or secret:find("%c") or (config.ownip and not radius_ipv4(config.ownip)) then
            return nil, "INVALID_RADIUS_SETTINGS"
        end
        return { auth = auth, cipher = cipher, key = "", pmf = pmf or 0,
            server = server, port = port, secret = secret }
    elseif auth ~= "OPEN" and auth ~= "OWE" then
        key = config.key
        local allow_hex = auth ~= "WPA3PSK" and auth ~= "WPA2PSKWPA3PSK"
        if type(key) ~= "string" or key:find("%c") or #key < 8 or
            (#key > 63 and not (allow_hex and #key == 64 and key:match("^%x+$"))) then
            return nil, "INVALID_PASSPHRASE"
        end
    end

    pmf = pmf or 0
    return { auth = auth, cipher = cipher, key = key, pmf = pmf }
end

function mtwifi_defs.security_signature(security)
    local signature = { security.auth, security.cipher, security.key, tostring(security.pmf) }
    if security.server then
        signature[5], signature[6], signature[7] = security.server, tostring(security.port), security.secret
    end
    return signature
end

mtwifi_defs.countryRegions = {
    -- CountryCode = 2g region, 5g region
    ["DB"] = { 5, 13 },
    ["AE"] = { 1, 0 },
    ["AL"] = { 1, 0 },
    ["AR"] = { 1, 3 },
    ["AT"] = { 1, 1 },
    ["AM"] = { 1, 2 },
    ["AU"] = { 1, 0 },
    ["AZ"] = { 1, 2 },
    ["BE"] = { 1, 1 },
    ["BH"] = { 1, 0 },
    ["BY"] = { 1, 0 },
    ["BO"] = { 1, 4 },
    ["BR"] = { 1, 1 },
    ["BN"] = { 1, 4 },
    ["BG"] = { 1, 1 },
    ["BZ"] = { 1, 4 },
    ["CA"] = { 0, 0 },
    ["CH"] = { 1, 1 },
    ["CL"] = { 1, 0 },
    ["CN"] = { 1, 0 },
    ["CO"] = { 0, 0 },
    ["CR"] = { 1, 0 },
    ["CY"] = { 1, 1 },
    ["CZ"] = { 1, 2 },
    ["DE"] = { 1, 1 },
    ["DK"] = { 1, 1 },
    ["DO"] = { 0, 0 },
    ["DZ"] = { 1, 0 },
    ["EC"] = { 1, 0 },
    ["EG"] = { 1, 2 },
    ["EE"] = { 1, 1 },
    ["ES"] = { 1, 1 },
    ["FI"] = { 1, 1 },
    ["FR"] = { 1, 2 },
    ["GE"] = { 1, 2 },
    ["GB"] = { 1, 1 },
    ["GR"] = { 1, 1 },
    ["GT"] = { 0, 0 },
    ["HN"] = { 1, 0 },
    ["HK"] = { 1, 0 },
    ["HU"] = { 1, 1 },
    ["HR"] = { 1, 2 },
    ["IS"] = { 1, 1 },
    ["IN"] = { 1, 0 },
    ["ID"] = { 1, 4 },
    ["IR"] = { 1, 4 },
    ["IE"] = { 1, 1 },
    ["IL"] = { 1, 0 },
    ["IT"] = { 1, 1 },
    ["JP"] = { 1, 9 },
    ["JO"] = { 1, 0 },
    ["KP"] = { 1, 5 },
    ["KR"] = { 1, 5 },
    ["KW"] = { 1, 0 },
    ["KZ"] = { 1, 0 },
    ["LB"] = { 1, 0 },
    ["LI"] = { 1, 1 },
    ["LT"] = { 1, 1 },
    ["LU"] = { 1, 1 },
    ["LV"] = { 1, 1 },
    ["MA"] = { 1, 0 },
    ["MC"] = { 1, 2 },
    ["MO"] = { 1, 0 },
    ["MK"] = { 1, 0 },
    ["MX"] = { 0, 0 },
    ["MY"] = { 1, 0 },
    ["NL"] = { 1, 1 },
    ["NO"] = { 0, 0 },
    ["NZ"] = { 1, 0 },
    ["OM"] = { 1, 0 },
    ["PA"] = { 0, 0 },
    ["PE"] = { 1, 4 },
    ["PH"] = { 1, 4 },
    ["PL"] = { 1, 1 },
    ["PK"] = { 1, 0 },
    ["PT"] = { 1, 1 },
    ["PR"] = { 0, 0 },
    ["QA"] = { 1, 0 },
    ["RO"] = { 1, 0 },
    ["RU"] = { 1, 0 },
    ["SA"] = { 1, 0 },
    ["SG"] = { 1, 0 },
    ["SK"] = { 1, 1 },
    ["SI"] = { 1, 1 },
    ["SV"] = { 1, 0 },
    ["SE"] = { 1, 1 },
    ["SY"] = { 1, 0 },
    ["TH"] = { 1, 0 },
    ["TN"] = { 1, 2 },
    ["TR"] = { 1, 2 },
    ["TT"] = { 1, 2 },
    ["TW"] = { 0, 3 },
    ["UA"] = { 1, 0 },
    ["US"] = { 0, 0 },
    ["UY"] = { 1, 5 },
    ["UZ"] = { 0, 1 },
    ["VE"] = { 1, 5 },
    ["VN"] = { 1, 0 },
    ["YE"] = { 1, 0 },
    ["ZA"] = { 1, 1 },
    ["ZW"] = { 1, 0 },
}

-- The 6 GHz region has its own numbering in the MediaTek channel tables.
-- Match the 6 GHz entries in mtwifi-scripts' countryRegions.
local full_band_6g = {
    AU = true, BR = true, CA = true, CO = true, CR = true, DB = true,
    DO = true, GB = true, GT = true, HN = true, HR = true, JP = true,
    KP = true, KR = true, KW = true, LB = true, MX = true, PE = true,
    PR = true, QA = true, SA = true, US = true,
}

function mtwifi_defs.country_region(country, band)
    local regions = mtwifi_defs.countryRegions[country]
    if country == "00" then regions = { 0, 0 } end
    if not regions then return nil end
    if band == "2g" then return regions[1] end
    if band == "5g" then return country == "US" and 26 or regions[2] end
    if band == "6g" then return (country == "00" or full_band_6g[country]) and 0 or 1 end
end

return mtwifi_defs
