#!/usr/bin/lua
-- Local interface between the inform client and the WEXT manager.
local json, uci, ubus = require("cjson"), require("uci"), require("ubus")
local l1, defs = require("l1dat_parser"), require("mtwifi_defs")
local fs = require("nixio.fs")
local fields = {"ssid", "encryption", "key", "ieee80211w", "hidden", "isolate"}
local bus = assert(ubus.connect())
local req = json.decode(io.read("*a"))
local cursor = uci.cursor(nil, arg[1])
local config = assert(cursor:get_all("be14_ucg", "main"))
assert(config.enabled == "1" or req.op == "overview", "agent disabled")

local function call(obj, method, data)
    return bus:call(obj, method, data or {}) or {}
end

local function selected()
    local selected = {}
    local names = config.section or {}
    if type(names) == "string" then names = {names} end
    for _, name in ipairs(names) do
        assert(name:match("^be14_ucg_[%w_]+$") and not selected[name], "invalid local AP binding")
        selected[name] = true
    end
    return selected
end

local codes = { ["2g"] = "ng", ["5g"] = "na", ["6g"] = "6e" }
local radio_names = { ng = "wifi0", na = "wifi1", ["6e"] = "wifi2" }

local function dbm(value)
    if type(value) ~= "number" then return end
    -- rpcd serializes dBm in a 32-bit ubus integer.
    if value >= 2^31 then value = value - 2^32 end
    if value < 0 and value >= -127 then return value end
end

local txt_fields = {["_airplay._tcp"]="model", ["_raop._tcp"]="am", ["_companion-link._tcp"]="rpmd",
    ["_device-info._tcp"]="model", ["_googlecast._tcp"]="md"}

local function label(value)
    if type(value) == "string" and #value <= 96 and value:find("%S") and not value:find("[%c<>]") then
        return value
    end
end

local function identities()
    local hosts = {}
    for mac, host in pairs(call("luci-rpc", "getHostHints")) do hosts[mac:lower()] = host end
    local owners, models = {}, {}
    for mac, host in pairs(hosts) do
        for _, field in ipairs({"ipaddrs", "ip6addrs"}) do
            for _, addr in ipairs(host[field] or {}) do
                if owners[addr] == nil then owners[addr] = mac
                elseif owners[addr] ~= mac then owners[addr] = false end
            end
        end
    end
    -- Match cached service advertisements by address. A service or host name alone
    -- is not a device model, and shared addresses must not identify a station.
    local services = call("umdns", "browse", {array=true, address=true})
    for service, field in pairs(txt_fields) do
        for _, record in pairs(services[service] or {}) do
            local owner
            for _, family in ipairs({"ipv4", "ipv6"}) do
                for _, addr in ipairs(record[family] or {}) do
                    local mac = owners[addr]
                    if mac ~= nil then
                        if owner == nil then owner = mac
                        elseif owner ~= mac then owner = false end
                    end
                end
            end
            if owner then
                for _, text in ipairs(record.txt or {}) do
                    local k, v = text:match("^([^=]+)=(.+)$")
                    if k and k:lower() == field and label(v) then
                        if models[owner] == nil then models[owner] = v
                        elseif models[owner] ~= v then models[owner] = false end
                    end
                end
            end
        end
    end
    for mac, host in pairs(hosts) do host.model = models[mac] or nil end
    return hosts
end

local function station_table(stations, hosts, ap)
    local rows, seen = {}, {}
    for _, sta in ipairs(stations) do
        local mac = type(sta.mac) == "string" and sta.mac:lower() or ""
        if mac:match("^%x%x:%x%x:%x%x:%x%x:%x%x:%x%x$") and mac ~= "00:00:00:00:00:00"
            and tonumber(mac:sub(1, 2), 16) % 2 == 0 and not seen[mac] then
            seen[mac] = true
            local host = hosts[mac] or {}
            local override = label(cursor:get("be14_ucg", "client_" .. mac:gsub(":", ""), "model"))
            local row = {mac=mac, vap_name=ap.name, radio=ap.radio, channel=ap.channel,
                uptime=sta.connected_time, signal=dbm(sta.signal), noise=dbm(sta.noise),
                ip=(host.ipaddrs or {})[1], hostname=label(host.name),
                device_name=override or host.model, model_source=override and "manual" or host.model and "mdns"}
            if #(host.ip6addrs or {}) > 0 then row.ipv6 = host.ip6addrs end
            if row.signal and row.noise then row.rssi = row.signal - row.noise end
            for _, dir in ipairs({"rx", "tx"}) do
                local stats = sta[dir] or {}
                for _, key in ipairs({"rate", "bytes", "packets"}) do
                    row[dir .. "_" .. key] = stats[key]
                end
                if stats.mcs then row[dir .. "_mcs"] = stats.mcs end
                if stats.nss and stats.nss > 0 then row[dir .. "_nss"] = stats.nss end
                for phy, flag in pairs({ht="is_11n", vht="is_11ac", he="is_11ax", eht="is_11be"}) do
                    if stats[phy] == true or stats[phy] == 1 then row[flag] = true end
                end
            end
            rows[#rows+1] = row
        end
    end
    return rows
end

local function snapshot()
    local profile = assert(l1.load_l1_profile(l1.L1_DAT_PATH))
    local status = call("network.wireless", "status")
    local radios, aps, bindings, candidates = {}, {}, {}, {}
    local hosts
    local owned = selected()
    for name, dev in pairs(profile.devname_ridx) do
        local band = l1.device_band(dev)
        local code = codes[band]
        if dev.INDEX == "MT7990" and code then
            assert(not radios[code], "ambiguous BE14 radio")
            local radio = status[name] or {}
            local row = { name = radio_names[code], radio = code, up = radio.up == true,
                ifname = dev.main_ifname, applied_htmode = (radio.config or {}).htmode }
            radios[code] = row
            local main = radio.up and call("iwinfo", "info", {device=dev.main_ifname}) or {}
            row.channel, row.tx_power = main.channel, main.txpower
            row.live_htmode = main.htmode
            local live = {}
            for _, v in ipairs(radio.interfaces or {}) do
                if radio.up and v.ifname and v.ifname:match("^" .. dev.ext_ifname .. "%d+$") then
                    live[v.section] = v
                end
            end
            cursor:foreach("wireless", "wifi-iface", function(c)
                if c.device ~= name or c.mode ~= "ap" then return end
                local section = c[".name"]
                if req.op == "overview" and section:match("^be14_ucg_[%w_]+$") and c.mldgroup == "0"
                    and not defs.disabled(c.mlo) and not defs.disabled(c.disabled) then
                    candidates[#candidates+1] = {section=section, band=code}
                end
                local vif = live[section]
                local iface = vif and vif.ifname
                local info = iface and call("iwinfo", "info", {device=iface}) or {}
                local observed = iface and info.ssid and info.ssid ~= "" and info.ssid or nil
                row.channel = row.channel or info.channel
                row.tx_power = row.tx_power or info.txpower
                row.live_htmode = row.live_htmode or info.htmode
                if owned[section] then
                    assert(c.mldgroup == "0" and not defs.disabled(c.mlo) and not defs.disabled(c.disabled),
                        "bind an enabled independent test AP with mldgroup 0")
                    assert(not bindings[code], "multiple AP bindings on one band")
                    local applied = {}
                    for _, k in ipairs(fields) do
                        local value = vif and vif.config and vif.config[k]
                        if value ~= nil then
                            applied[k] = value == true and "1" or value == false and "0" or tostring(value)
                        end
                    end
                    bindings[code] = {section=section, ssid=c.ssid, observed=observed, applied=applied,
                        encryption=c.encryption, ieee80211w=c.ieee80211w, key=c.key, isolate=c.isolate,
                        hidden=c.hidden, device=name}
                    owned[section] = nil
                end
                if observed and info.bssid then
                    local ap = {section=section, name=iface, radio=code, radio_name=row.name,
                        essid=observed, bssid=info.bssid:lower(), channel=info.channel,
                        tx_power=info.txpower or row.tx_power, up=true}
                    local stations = call("iwinfo", "assoclist", {device=iface}).results
                    if type(stations) == "table" then
                        if #stations > 0 and not hosts then hosts = identities() end
                        ap.sta_table = station_table(stations, hosts or {}, ap)
                    end
                    aps[#aps+1] = ap
                elseif iface then
                    row.incomplete_associations = true
                end
            end)
        end
    end
    assert(radios.ng and radios.na and radios["6e"], "BE14 tri-band L1 profile required")
    assert(not next(owned), "bound test AP missing")
    local net = assert(config.management_network)
    assert(net:match("^[%w_]+$"), "invalid management network")
    local iface = call("network.interface." .. net, "status")
    local dev = iface.l3_device or iface.device
    if not (dev and dev:match("^[%w_.-]+$")) then
        assert(req.op == "overview", "management interface unavailable")
        dev = nil
    end
    local addr = (iface["ipv4-address"] or {})[1]
    local mac = dev and fs.readfile("/sys/class/net/" .. dev .. "/address")
    return {radios=radios, aps=aps, bindings=bindings, mac=mac and mac:match("%S+"),
        management_device=dev, ip=addr and addr.address, url=config.inform_url, wlan_id=config.wlan_id or "",
        firmware_version=config.firmware_version, candidates=candidates, sshd_port=tonumber(config.sshd_port or 22)}
end

if req.op == "snapshot" or req.op == "overview" then
    print(json.encode(snapshot()))
elseif req.op == "settings" then
    print(json.encode({url=config.inform_url}))
elseif req.op == "replace" then
    -- Compare all fields we own and validate all bindings before staging one UCI commit.
    local normal = uci.cursor()
    assert(not next(normal:changes("wireless") or {}), "pending local wireless edits")
    local state = snapshot()
    local allowed = {}
    for _, k in ipairs(fields) do allowed[k] = true end
    for code, change in pairs(req.changes) do
        local current = assert(state.bindings[code])
        assert(current.section == change.section and current.device == change.device, "AP binding changed")
        for k in pairs(allowed) do
            local now = cursor:get("wireless", current.section, k)
            assert((now or false) == (change.before[k] or false), "AP changed locally")
        end
        for k in pairs(change.after) do assert(allowed[k], "unsupported AP setting") end
    end
    for _, c in pairs(req.changes) do
        for k in pairs(allowed) do
            if c.after[k] then
                assert(cursor:set("wireless", c.section, k, c.after[k]))
            elseif c.before[k] then
                assert(cursor:delete("wireless", c.section, k))
            end
        end
    end
    assert(cursor:commit("wireless"), "wireless commit failed")
    print("{}")
else
    error("unknown local operation")
end
