"""Operating radio modes and per-BSS counters for the BE14 WEXT driver."""
from array import array
from collections import Counter
import fcntl
from pathlib import Path
import re
import socket
import struct

# The BE14 WEXT stack supports SAE and OWE on its AP interfaces. Network
# checks these per-radio flags before assigning WPA3 WLANs to a device.
RADIO_CAPS2_SAE = 1 << 0
RADIO_CAPS2_OWE = 1 << 3


def wext_radio(ifname, band):
    # Use the same read-only MTK ioctls as libiwinfo's mtk_get_htmode.
    # The cfg80211 compatibility interface can return HT20 for an EHT radio.
    if not isinstance(ifname, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,15}", ifname):
        return None, None

    def query(sock, oid, value):
        request = struct.pack("@16sPHH", ifname.encode(), value.buffer_info()[0],
                              value.itemsize, oid).ljust(32, b"\0")
        try:
            fcntl.ioctl(sock, 0x8BE1, request)  # RT_PRIV_IOCTL
        except OSError:
            return None
        return value[0]

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            mask = query(sock, 0x050C, array("L", [0]))  # RT_OID_802_11_PHY_MODE
            bw = query(sock, 0x1903, array("B", [255]))  # OID_802_11_BW
    except OSError:
        return None, None
    # BAND_WIDTH_8080 occupies two 80 MHz segments.
    width = {0: 20, 1: 40, 2: 80, 3: 160, 4: 10, 5: 5, 6: 160, 7: 320}.get(bw)
    if not mask:
        return None, width
    # WMODE_* bits from the driver's per-band PhyMode.
    be, ax = {"ng": (9, 6), "na": (10, 7), "6e": (11, 8)}[band]
    if mask & (1 << be):
        return "EHT", width
    if mask & (1 << ax):
        return "HE", width
    if band == "na" and mask & (1 << 5):
        return "VHT", width
    if band != "6e":
        return ("HT" if mask & 0x18 else "NOHT"), width
    return None, width


def parse_htmode(value):
    if value == "NOHT":
        return "NOHT", 20
    match = re.fullmatch(r"(EHT|HE|VHT|HT)(20|40|80|160|320)(\+80|_80|[+-]|-[12])?", value or "")
    if not match:
        return None, None
    family, width, suffix = match.groups()
    return family, 160 if width == "80" and suffix in ("+80", "_80") else int(width)


def radio_mode(row):
    applied, configured_width = parse_htmode(row.get("applied_htmode"))
    live, live_width = parse_htmode(row.get("live_htmode"))
    if row.get("radio") == "6e":
        if applied not in ("HE", "EHT"):
            applied, configured_width = None, None
        if live not in ("HE", "EHT"):
            live, live_width = None, None
    if not row.get("up"):
        return applied, configured_width, None
    if live and live == applied:
        return live, configured_width, live_width
    mode, width = wext_radio(row.get("ifname"), row["radio"])
    # Some WEXT builds expose the cfg80211 HT20 placeholder on every band.
    if (live, live_width) == ("HT", 20) and applied in ("VHT", "HE", "EHT"):
        live, live_width = None, None
    family = mode or live or applied
    if width is None and live == family:
        width = live_width
    return family, configured_width, width


def interface_counters(ifnames):
    # /proc/net/dev and sysfs expose the same kernel net_device counters.
    # Read each active BSS once and retain full 64-bit integer precision.
    counters = {}
    try:
        lines = Path("/proc/net/dev").read_text().splitlines()
    except OSError:
        lines = []
    for line in lines:
        name, _, values = line.rpartition(":")
        name = name.strip()
        if name not in ifnames:
            continue
        values = values.split()
        if len(values) < 16 or not all(value.isdecimal() for value in values):
            continue
        counters[name] = {f"{direction}_{field}": int(values[offset + i])
                          for direction, offset in (("rx", 0), ("tx", 8))
                          for i, field in enumerate(("bytes", "packets", "errors", "dropped"))}
    for name in ifnames - counters.keys():
        if not re.fullmatch(r"[a-zA-Z0-9_-][a-zA-Z0-9_.-]{0,14}", name):
            continue
        stats = Path("/sys/class/net") / name / "statistics"
        for direction in ("rx", "tx"):
            for field in ("bytes", "packets", "errors", "dropped"):
                key = f"{direction}_{field}"
                try:
                    value = (stats / key).read_text().strip()
                except OSError:
                    continue
                if value.isdecimal():
                    counters.setdefault(name, {})[key] = int(value)
    return counters


def wireless_report(snapshot, learned):
    aps = sorted(snapshot["aps"], key=lambda ap: ap["name"])
    counters = interface_counters({ap["name"] for ap in aps})
    names = Counter((ap["radio"], ap["essid"]) for ap in aps)
    clients = {band: set() for band in snapshot["radios"]}
    incomplete = {band for band, row in snapshot["radios"].items() if row.get("incomplete_associations")}
    for ap in aps:
        if "sta_table" in ap:
            clients[ap["radio"]].update(sta["mac"] for sta in ap["sta_table"])
        else:
            incomplete.add(ap["radio"])

    radios, stats, vaps = [], [], []
    widths = {}
    for band in ("ng", "na", "6e"):
        row = snapshot["radios"][band]
        mode, configured_width, widths[band] = radio_mode(row)
        radio = {k: row[k] for k in ("name", "radio", "channel", "tx_power", "up") if k in row}
        radio["radio_caps2"] = RADIO_CAPS2_SAE | RADIO_CAPS2_OWE
        if configured_width:
            radio["ht"] = str(configured_width)
        if mode:
            radio.update(is_11be=mode == "EHT", is_11ax=mode in ("HE", "EHT"),
                         is_11ac=band == "na" and mode in ("VHT", "HE", "EHT"))
        current = {k: radio[k] for k in ("name", "radio", "channel", "tx_power", "up") if k in radio}
        if widths[band] is not None and row.get("up"):
            current["bw"] = widths[band]
        if band not in incomplete:
            current["num_sta"] = radio["num_sta"] = len(clients[band])
        radios.append(radio)
        stats.append(current)

    for ap in aps:
        band, ssid = ap["radio"], ap["essid"]
        entry = {k: ap[k] for k in ("name", "radio", "radio_name", "essid", "bssid",
                                    "channel", "up", "tx_power") if k in ap}
        entry["usage"] = "user"
        bound = snapshot["bindings"].get(band, {})
        if not snapshot.get("automatic") and bound.get("section") == ap["section"] and snapshot["wlan_id"]:
            entry["id"] = snapshot["wlan_id"]
        elif names[band, ssid] == 1:
            # The radio can still be broadcasting the old WLAN while a new binding is pending.
            ident = learned.get(band, {}).get(ssid)
            if ident:
                entry["id"] = ident
        # Network derives operating channel, width and AP traffic from active VAPs,
        # including local BSSes for which it has no WLAN record.
        if widths[band] is not None:
            entry["bw"] = widths[band]
        if "sta_table" in ap:
            entry["sta_table"] = []
            for station in ap["sta_table"] or []:  # lua-cjson represents an empty array as {}.
                station = station.copy()
                station.pop("model_source", None)
                # Network retains each direction separately. Only update its
                # balance inputs together, from the same association sample.
                if not (station.get("rx_rate") and station.get("tx_rate")):
                    station.pop("rx_rate", None)
                    station.pop("tx_rate", None)
                model = station.get("device_name")
                if model:
                    # Network indexes the AP station's hostname for its client list.
                    # Keep the actual host name alongside an independently known model.
                    host = station.get("hostname")
                    if not host:
                        station["hostname"] = model
                    elif model.casefold() not in host.casefold():
                        station["hostname"] = f"{host} ({model})"
                entry["sta_table"].append(station)
            entry["num_sta"] = len(entry["sta_table"])
        entry.update(counters.get(ap["name"], {}))
        vaps.append(entry)
    report = dict(radio_table=radios, radio_table_stats=stats, vap_table=vaps)
    # Network consumes identification separately from the VAP's association row.
    # Publish only advertised or explicitly configured models.
    models = {}
    for ap in vaps:
        for sta in ap.get("sta_table", []):
            if sta.get("device_name"):
                models[sta["mac"]] = sta["device_name"]
    if models:
        report["fingerprints"] = {"hosts": [
            {"mac": mac, "custom": {"device_name": model, "product_model": model}}
            for mac, model in sorted(models.items())], "version": {"custom": "be14-1"}}
    if not incomplete:
        report["num_sta"] = len(set().union(*clients.values()))
    return report
