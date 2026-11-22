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
local automatic = config.auto_provision == "1"
assert(config.enabled == "1" or req.op == "overview", "agent disabled")

local function call(obj, method, data)
    return bus:call(obj, method, data or {}) or {}
end

local function selected()
    local selected = {}
    local names = config.section or {}
    if type(names) == "string" then names = {names} end
    for _, name in ipairs(names) do
        assert(name:match(automatic and "^[%w_]+$" or "^be14_ucg_[%w_]+$") and not selected[name], "invalid local AP binding")
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
                row[dir .. "_bytes"] = stats.bytes
                row[dir .. "_packets"] = stats.packets
                if stats.mcs then row[dir .. "_mcs"] = stats.mcs end
                if stats.nss and stats.nss > 0 then row[dir .. "_nss"] = stats.nss end
                local modern = false
                for phy, flag in pairs({ht="is_11n", vht="is_11ac", he="is_11ax", eht="is_11be"}) do
                    if stats[phy] == true or stats[phy] == 1 then
                        row[flag], modern = true, true
                    end
                end
                -- rpcd emits zero without a PHY sample. Older WEXT tables
                -- can also decode an empty rate word as legacy CCK 1 Mbit/s.
                local rate = stats.rate
                if type(rate) == "number" and rate > 0 and (ap.radio == "ng" or modern or rate ~= 1000) then
                    row[dir .. "_rate"] = rate
                end
            end
            rows[#rows+1] = row
        end
    end
    return rows
end

-- A driver assigns AP indices in UCI order. Insert or enable only after the last
-- active AP, so existing BSSes keep their interface names and primary BSSID.
local function secondary_slots(name, code, dev, radio, live)
    local aps, last, main_live = {}, 0, false
    cursor:foreach("wireless", "wifi-iface", function(c)
        if c.device == name and c.mode == "ap" then
            aps[#aps+1] = c
            if not defs.disabled(c.disabled) then last = #aps end
            if live[c[".name"]] and live[c[".name"]].ifname == dev.main_ifname then main_live = true end
        end
    end)
    local options, managed, active, first = {}, {}, 0, nil
    for index, c in ipairs(aps) do
        if not defs.disabled(c.disabled) then
            active = active + 1
            first = first or index
        end
    end
    local spare = radio.up and main_live and active < defs.max_mbssid and
        fs.stat("/sys/class/net/" .. dev.ext_ifname .. active) ~= nil
    for index, c in ipairs(aps) do
        local section = c[".name"]
        local network = c.network
        if type(network) == "table" and #network == 1 then network = network[1] end
        if not c[".anonymous"] and (network == nil or type(network) == "string") and
                (c.mldgroup == nil or c.mldgroup == "0") and not defs.disabled(c.mlo) then
            local owned = c.be14_ucg_managed == "1"
            local running = live[section]
            managed[section] = owned and network == config.management_network and first and index > first
                and (not running or running.ifname ~= dev.main_ifname)
            if (managed[section] and running) or
                    (spare and index > last and defs.disabled(c.disabled)) then
                options[section] = {section=section, band=code, ssid=c.ssid}
            end
        end
    end
    local new = "be14_ucg_auto_" .. code
    if spare and not cursor:get("wireless", new) then
        options[new] = {section=new, band=code, create=true}
    end
    return options, new, managed
end

-- netifd can put several logical interfaces (often wan and wan6) on one bridge.
local function address_services(net, iface, dev)
    local conflicts = {}
    local enabled = {["1"]=true, on=true, yes=true, ["true"]=true, enabled=true}
    local main_dhcp = false
    cursor:foreach("dhcp", "odhcpd", function(c)
        if c.maindhcp ~= nil then main_dhcp = enabled[c.maindhcp] end
    end)
    cursor:foreach("dhcp", "dhcp", function(s)
        local name = s.interface or s[".name"]
        if type(name) ~= "string" or name == "" then return end
        local owner = name == net and iface or call("network.interface." .. name, "status")
        if (owner.l3_device or owner.device) ~= dev then return end

        local row = {interface=name}
        -- dnsmasq creates IPv4 ranges only for static networks with an IPv4 subnet.
        -- Its ignore flag does not turn off odhcpd's explicitly configured services.
        local addr = (owner["ipv4-address"] or {})[1]
        local pool = s.interface and owner.proto == "static" and addr and addr.address and
            not enabled[s.ignore] and s.dhcpv4 ~= "disabled"
        if pool or (main_dhcp and s.dhcpv4 == "server") then row.dhcpv4 = "server" end
        for _, key in ipairs({"ra", "dhcpv6"}) do
            local mode = s[key]
            -- A relay master listens upstream. A downstream hybrid may act as a server.
            if mode == "server" or (not enabled[s.master] and (mode == "hybrid" or mode == "relay")) then
                row[key] = mode
            end
        end
        if row.dhcpv4 or row.ra or row.dhcpv6 then conflicts[#conflicts+1] = row end
    end)
    return conflicts
end

local function snapshot()
    local profile = assert(l1.load_l1_profile(l1.L1_DAT_PATH))
    local status = call("network.wireless", "status")
    local radios, aps, bindings, candidates = {}, {}, {}, {}
    local slots, other_ssids = {}, {}
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
            local options, new, managed = {}, nil, {}
            if automatic or req.op == "overview" then
                options, new, managed = secondary_slots(name, code, dev, radio, live)
                for _, option in pairs(options) do slots[#slots+1] = option end
            end
            cursor:foreach("wireless", "wifi-iface", function(c)
                if c.device ~= name or c.mode ~= "ap" then return end
                local section = c[".name"]
                if req.op == "overview" and live[section] and live[section].ifname ~= dev.main_ifname
                    and section:match("^be14_ucg_[%w_]+$") and c.mldgroup == "0"
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
                if c.ssid and not defs.disabled(c.disabled) and not (automatic and owned[section]) then
                    other_ssids[c.ssid] = true
                    if observed then other_ssids[observed] = true end
                end
                if owned[section] then
                    assert(automatic or (c.mldgroup == "0" and not defs.disabled(c.mlo) and not defs.disabled(c.disabled)),
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
                    if automatic then
                        bindings[code].eligible = options[section] ~= nil
                        bindings[code].managed_secondary = managed[section] == true
                        bindings[code].disabled = c.disabled
                        bindings[code].be14_ucg_managed = c.be14_ucg_managed
                        bindings[code].network = c.network
                        local master = iface and fs.readlink("/sys/class/net/" .. iface .. "/master")
                        bindings[code].master = master and master:match("([^/]+)$")
                    end
                    owned[section] = nil
                end
                -- Provisioning needs the live BSS and bridge, not station discovery.
                if req.stations == false then return end
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
            if automatic and owned[new] and not cursor:get("wireless", new) then
                assert(not bindings[code], "multiple AP bindings on one band")
                bindings[code] = {section=new, device=name, missing=true, eligible=options[new] ~= nil}
                owned[new] = nil
            end
        end
    end
    assert(radios.ng and radios.na and radios["6e"], "BE14 tri-band L1 profile required")
    assert(automatic or not next(owned), "bound test AP missing")
    local net = assert(req.op == "overview" and req.network or config.management_network)
    assert(net:match("^[%w_]+$"), "invalid management network")
    local iface = call("network.interface." .. net, "status")
    local dev = iface.l3_device or iface.device
    if not (dev and dev:match("^[%w_.-]+$")) then
        assert(req.op == "overview", "management interface unavailable")
        dev = nil
    end
    local addr = (iface["ipv4-address"] or {})[1]
    local upstream_error, conflicts
    if not dev then
        upstream_error = "upstream_device"
    elseif fs.stat("/sys/class/net/" .. dev .. "/bridge", "type") ~= "dir" then
        upstream_error = "upstream_bridge"
    else
        conflicts = address_services(net, iface, dev)
        if #conflicts > 0 then upstream_error = "upstream_dhcp" end
    end
    local gateways = {}
    for _, route in ipairs(iface.route or {}) do
        if route.target == "0.0.0.0" and tonumber(route.mask) == 0 and route.nexthop then
            gateways[#gateways+1] = route.nexthop
        end
    end
    local mac = dev and fs.readfile("/sys/class/net/" .. dev .. "/address")
    for _, binding in pairs(bindings) do
        binding.bridge_attached = dev ~= nil and binding.master == dev
    end
    return {radios=radios, aps=aps, bindings=bindings, mac=mac and mac:match("%S+"),
        management_device=dev, ip=addr and addr.address, url=config.inform_url, wlan_id=config.wlan_id or "",
        firmware_version=config.firmware_version, candidates=candidates, sshd_port=tonumber(config.sshd_port or 22),
        automatic=automatic, slots=slots, other_ssids=other_ssids, missing_selection=next(owned) ~= nil,
        management_network=net, upstream_bridge=upstream_error == nil, upstream_error=upstream_error,
        upstream_conflicts=upstream_error == "upstream_dhcp" and conflicts or nil,
        upstream_gateways=gateways, wired_host_enabled=config.wired_host == "1"}
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
    assert(state.wlan_id == req.wlan_id, "WLAN binding changed")
    assert(automatic == (req.automatic == true), "WLAN management mode changed")
    if automatic then
        assert(not state.missing_selection and state.management_network == req.network, "upstream binding changed")
    end
    local allowed = {}
    for _, k in ipairs(fields) do allowed[k] = true end
    if automatic then allowed.disabled = true; allowed.be14_ucg_managed = true; allowed.network = true end
    for code, change in pairs(req.changes) do
        local current = assert(state.bindings[code])
        assert(current.section == change.section and current.device == change.device, "AP binding changed")
        if automatic then
            if req.rollback or change.after.disabled == "1" then
                assert(current.managed_secondary, "AP ownership changed")
            else
                assert(state.upstream_bridge and current.eligible and not state.other_ssids[change.after.ssid],
                    "secondary AP unavailable or duplicate SSID")
            end
            if change.create then assert(current.missing and current.section == "be14_ucg_auto_" .. code) end
            if change.remove then assert(req.rollback and current.section == "be14_ucg_auto_" .. code) end
        end
        for k in pairs(allowed) do
            local now = cursor:get("wireless", current.section, k)
            local was = change.before[k]
            if type(now) == "table" and type(was) == "table" then
                assert(#now == 1 and #was == 1 and now[1] == was[1], "AP network changed locally")
            else
                assert((now or false) == (was or false), "AP changed locally")
            end
        end
        for k in pairs(change.after) do assert(allowed[k], "unsupported AP setting") end
    end
    for _, c in pairs(req.changes) do
        if automatic and c.remove then
            assert(cursor:delete("wireless", c.section))
        else
            if automatic and c.create then
                assert(cursor:set("wireless", c.section, "wifi-iface"))
                assert(cursor:set("wireless", c.section, "mode", "ap"))
                assert(cursor:set("wireless", c.section, "device", c.device))
                assert(cursor:set("wireless", c.section, "mldgroup", "0"))
            end
            for k in pairs(allowed) do
                if c.after[k] then
                    assert(cursor:set("wireless", c.section, k, c.after[k]))
                elseif c.before[k] then
                    assert(cursor:delete("wireless", c.section, k))
                end
            end
        end
    end
    assert(cursor:commit("wireless"), "wireless commit failed")
    print("{}")
else
    error("unknown local operation")
end
