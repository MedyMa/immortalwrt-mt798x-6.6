local M = {}

local wireless_modes = {
    ["2g"] = { NOHT = 0, HT = 9, HE = 16, EHT = 22 },
    ["5g"] = { NOHT = 2, HT = 8, VHT = 14, HE = 17, EHT = 23 },
    ["6g"] = { HE = 18, EHT = 24 }
}

local function channels(step, ...)
    local result = {}
    local ranges = { ... }
    for i = 1, #ranges, 2 do
        for channel = ranges[i], ranges[i + 1], step do
            result[#result + 1] = channel
        end
    end
    return result
end

-- Country_Region*_ChDesc in the private driver. Used for offline selectors;
-- the running driver's channel list can be narrower due to hardware or DFS.
M.region_channels = {
    ["2g"] = {
        ["0"] = channels(1, 1, 11), ["1"] = channels(1, 1, 13),
        ["5"] = channels(1, 1, 14),
    },
    ["5g"] = {
        ["0"] = channels(4, 36, 64, 149, 165),
        ["1"] = channels(4, 36, 64, 100, 140),
        ["2"] = channels(4, 36, 64),
        ["3"] = channels(4, 52, 64, 149, 161),
        ["4"] = channels(4, 149, 165),
        ["5"] = channels(4, 149, 161),
        ["9"] = channels(4, 36, 64, 100, 116, 132, 140, 149, 165),
        ["13"] = channels(4, 36, 64, 100, 144, 149, 165),
        ["26"] = channels(4, 36, 64, 100, 144, 149, 177),
    },
    ["6g"] = { ["0"] = channels(4, 1, 233), ["1"] = channels(4, 1, 93) },
}

local all_channels = {
    ["2g"] = M.region_channels["2g"]["5"],
    ["5g"] = channels(4, 34, 46, 36, 64, 100, 144, 149, 177, 184, 196),
    ["6g"] = M.region_channels["6g"]["0"],
}

function M.valid_channel(band, value)
    if not tostring(value or ""):match("^[1-9]%d*$") then return false end
    local channel = tonumber(value)
    for _, candidate in ipairs(all_channels[band] or {}) do
        if channel == candidate then return true end
    end
    return band == "6g" and channel == 2
end

local function radio_channels(band, dats)
    local region = tonumber(dats[band == "2g" and "CountryRegion" or "CountryRegionABand"])
    if band == "5g" and dats.CountryCode == "US" then region = 26 end
    return M.region_channels[band][tostring(region and region % 128)]
end

local function has_block(available, first, count)
    if not available then return true end
    for channel = first, first + (count - 1) * 4, 4 do
        if not available[channel] then return false end
    end
    return true
end

local function center320(channel, layout, available)
    for index = 0, 5 do
        local first = 1 + index * 32
        if (layout == 0 or index % 2 + 1 == layout) and
            channel >= first and channel <= first + 60 and (channel - first) % 4 == 0 and
            has_block(available, first, 16) then
            return first + 30
        end
    end
end

local function channel_index(channels)
    if not channels then return nil end
    local index = {}
    for _, channel in ipairs(channels) do index[channel] = true end
    return index
end

local function channel_set(band, values, allow_ranges)
    local set = {}
    if values == nil or values == "" then return set end
    if type(values) ~= "table" then values = { values } end
    for _, value in ipairs(values) do
        if type(value) ~= "string" and type(value) ~= "number" then return nil end
        for token in tostring(value):gmatch("[^%s,;]+") do
            local first, last = token:match("^([1-9]%d*)%-([1-9]%d*)$")
            if allow_ranges and first and M.valid_channel(band, first) and
                M.valid_channel(band, last) and tonumber(first) <= tonumber(last) then
                for channel = tonumber(first), tonumber(last) do
                    if M.valid_channel(band, channel) then set[channel] = true end
                end
            elseif M.valid_channel(band, token) then
                set[tonumber(token)] = true
            else
                return nil
            end
        end
    end
    return set
end

function M.set_channel(config, dats)
    local automatic = config.channel == "auto"
    dats.AutoChannelSelect = automatic and 3 or 0
    dats.Channel = automatic and 0 or config.channel
    dats.AcsPrimaryChannels = ""
    if not automatic then return true end

    local band = config.band
    local allowed = channel_set(band, config.chanlist, true)
    local excluded = channel_set(band, config.acs_skiplist, false)
    if not allowed or not excluded then return nil, "INVALID_ACS_CHANNEL_LIST" end
    local restricted = next(allowed) ~= nil
    local skip_dfs = band == "5g" and (config.acs_exclude_dfs == true or
        tostring(config.acs_exclude_dfs) == "1" or config.doth == "0" or config.dfs == "0")
    local layout = dats.Eht320Layout or 0
    if not restricted and not next(excluded) and not skip_dfs and layout == 0 then return true end

    local candidates = {}
    local region_list = radio_channels(band, dats)
    local available = channel_index(region_list)
    local template_skip = channel_set(band, dats.AutoChannelSkipList, false) or {}
    for _, channel in ipairs(region_list or all_channels[band]) do
        local dfs = channel >= 52 and channel <= 144
        if (not restricted or allowed[channel]) and not excluded[channel] and not template_skip[channel] and
            not (skip_dfs and dfs) and (layout == 0 or center320(channel, layout, available)) then
            candidates[#candidates + 1] = channel
        end
    end
    if #candidates == 0 then return nil, "ACS_NO_ELIGIBLE_CHANNEL" end
    dats.AcsPrimaryChannels = table.concat(candidates, ";")
    return true
end

local function center80(band, channel)
    if type(channel) == "string" and not channel:match("^%d+$") then return nil end
    channel = tonumber(channel)
    if not channel then return nil end
    if band == "6g" and channel >= 1 and channel <= 221 and channel % 4 == 1 then
        return 7 + 16 * math.floor((channel - 1) / 16)
    elseif band == "5g" then
        for _, center in ipairs({42, 58, 106, 122, 138, 155, 171}) do
            if channel >= center - 6 and channel <= center + 6 and (channel - center + 6) % 4 == 0 then
                return center
            end
        end
    end
end

function M.apply(config, dats)
    local htmode = config.htmode
    local family, width, suffix
    if htmode == "NOHT" then
        family, width, suffix = "NOHT", 20, ""
    elseif type(htmode) == "string" then
        family, width, suffix = htmode:match("^(%u+)(%d+)(.*)$")
        width = tonumber(width)
    end
    local split = suffix == "_80" and width == 80 and (family == "VHT" or family == "HE")
    local layout = family == "EHT" and width == 320 and
        tonumber(suffix:match("^%-([12])$")) or 0
    local modes = wireless_modes[config.band]
    local wmode = modes and modes[family] or nil
    if wmode == nil or not width or
        (width ~= 20 and width ~= 40 and width ~= 80 and width ~= 160 and width ~= 320) or
        (suffix ~= "" and not split and layout == 0 and not (width == 40 and (suffix == "+" or suffix == "-"))) or
        (family == "HT" and width > 40) or
        (config.band == "2g" and width > 40) or
        (width == 320 and (config.band ~= "6g" or family ~= "EHT")) then
        return nil, "invalid mode or bandwidth for this band"
    end
    local primary = tonumber(config.channel)
    local available = channel_index(radio_channels(config.band, dats))
    if config.channel ~= "auto" and (not M.valid_channel(config.band, config.channel) or
        (available and not available[primary]) or (width == 320 and not center320(primary, layout, available))) then
        return nil, "invalid primary channel for this band or bandwidth"
    end
    if config.band == "6g" and tonumber(config.channel) == 2 and width ~= 20 then
        return nil, "6 GHz channel 2 requires a 20 MHz channel"
    end
    if split then
        local first, second = center80(config.band, config.channel), center80(config.band, config.channel2)
        if not first or not second or math.abs(first - second) <= 16 or
            not has_block(available, first - 6, 4) or not has_block(available, second - 6, 4) then
            return nil, "80+80 MHz requires two fixed, non-adjacent 80 MHz channel segments"
        end
    end
    if family == "NOHT" and config.band == "2g" and config.hwmode == "11b" then
        wmode = 1
    end

    dats.HT_BW, dats.VHT_BW, dats.EHT_ApBw, dats.Eht320Layout = 0, 0, 0, layout
    if width >= 40 then
        dats.HT_BW = 1
        dats.VHT_BW = width >= 160 and 2 or width == 80 and 1 or 0
        if family == "EHT" then
            dats.EHT_ApBw = ({ [40] = 1, [80] = 2, [160] = 3, [320] = 4 })[width]
        end
    end
    if width == 40 then
        local noscan = config.noscan
        dats.HT_BSSCoexistence = (noscan == true or noscan == 1 or noscan == "1") and 0 or 1
    end
    if suffix == "+" then
        dats.HT_EXTCHA = 1
    elseif suffix == "-" or width == 320 then
        dats.HT_EXTCHA = 0
    end
    local secondary = {}
    for i = 1, tonumber(dats.BssidNum) or 1 do
        secondary[i] = split and tostring(config.channel2) or "0"
    end
    dats.VHT_Sec80_Channel = table.concat(secondary, ";")
    if split then dats.VHT_BW = 3 end

    return true, nil, wmode
end

return M
