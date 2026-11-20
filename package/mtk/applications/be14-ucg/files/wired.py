"""Resolve the physical Ethernet port on the configured inform route."""
import ipaddress
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import urlsplit

NET = Path("/sys/class/net")


def read(path):
    try:
        return path.read_text().strip()
    except (OSError, UnicodeError):
        return ""


def net_json(*command):
    try:
        result = subprocess.run(command, check=True, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, timeout=2)
        rows = json.loads(result.stdout)
        return rows if isinstance(rows, list) else []
    except (OSError, subprocess.SubprocessError, ValueError):
        return []


def interface(name):
    return isinstance(name, str) and re.fullmatch(r"[a-zA-Z0-9_-][a-zA-Z0-9_.-]{0,14}", name)


def unicast(mac):
    return (isinstance(mac, str) and re.fullmatch(r"(?:[\da-f]{2}:){5}[\da-f]{2}", mac, re.ASCII)
            and mac != "00:00:00:00:00:00" and not int(mac[:2], 16) & 1)


def bridge_port(bridge, next_hop, logical, vlan):
    try:
        members = {port.name for port in (NET / bridge / "brif").iterdir() if interface(port.name)}
    except OSError:
        return None
    # A bridge with both SFP and copper connected needs the learned egress port.
    # The next hop is used for lookup only; it need not be the adjacent switch.
    if len(members) > 1:
        reachable = set()
        for n in net_json("ip", "-j", "neigh", "show", "to", next_hop, "dev", logical):
            state = n.get("state", [])
            states = state.split() if isinstance(state, str) else state
            mac = n.get("lladdr", "").lower()
            if n.get("dst") == next_hop and unicast(mac) and not {"FAILED", "INCOMPLETE"}.intersection(states):
                reachable.add(mac)
        if reachable:
            ports = {row.get("ifname") or row.get("dev") for row in net_json("bridge", "-j", "fdb", "show", "br", bridge)
                     if row.get("mac", "").lower() in reachable and row.get("state") != "permanent"
                     and not {"self", "local"}.intersection(row.get("flags", []))
                     and (vlan is None or str(row.get("vlan", "")) == str(vlan))}
            proven = ports & members
            if len(proven) == 1:
                return proven.pop()
            if proven:
                return None
        # Count all members, including wireless: never choose Ethernet merely
        # because it is the only Ethernet port in a mixed bridge.
        carriers = {m: read(NET / m / "carrier") for m in members}
        if any(v not in ("0", "1") for v in carriers.values()):
            return None
        members = {m for m, carrier in carriers.items() if carrier == "1"}
    return members.pop() if len(members) == 1 else None


def wired_uplink(snapshot):
    logical = snapshot.get("management_device")
    if not interface(logical):
        return {}
    try:
        host = str(ipaddress.ip_address(urlsplit(snapshot.get("url") or "").hostname))
    except ValueError:
        return {}
    routes = net_json("ip", "-j", "route", "get", host)
    if len(routes) != 1 or routes[0].get("dev") != logical:
        return {}
    next_hop = routes[0].get("gateway") or host
    name, vlan = logical, None
    # Resolve VLANs and bridges, preserving DSA user ports rather than following
    # their CPU-facing lower link. Unsupported tunnels and bonds remain unknown.
    for _ in range(5):
        root = NET / name
        if (root / "bridge").is_dir():
            name = bridge_port(name, next_hop, logical, vlan)
            if not name:
                return {}
            continue
        links = net_json("ip", "-j", "-d", "link", "show", "dev", name)
        info = links[0] if len(links) == 1 else {}
        details = info.get("linkinfo", {})
        kind = details.get("info_kind")
        if kind == "vlan" and interface(info.get("link")):
            vlan = details.get("info_data", {}).get("id")
            name = info["link"]
            continue
        if (kind not in (None, "dsa") or not (root / "device").exists() or read(root / "type") != "1"
                or (root / "wireless").exists() or (root / "phy80211").exists()
                or name in {ap["name"] for ap in snapshot.get("aps", [])}
                or re.fullmatch(r"(?:ra[i-x]?|apcli\w*|wds\w*|mesh\w*|wlan)\d+", name)):
            return {}
        carrier = read(root / "carrier")
        if carrier not in ("0", "1"):
            return {}
        row = {"name": name, "up": carrier == "1"}
        mac = read(root / "address").lower()
        if unicast(mac):
            row["mac"] = mac
        if row["up"]:
            speed = read(root / "speed")
            if speed.isdecimal() and 0 < int(speed) < 1_000_000:
                row["speed"] = int(speed)
            duplex = read(root / "duplex").lower()
            if duplex in ("full", "half"):
                row["full_duplex"] = duplex == "full"
        return row
    return {}


def uplink_report(snapshot):
    link = wired_uplink(snapshot)
    # Network substitutes the E7 catalog's maximum rate if speed is missing.
    # Send an operating Ethernet interface only when its rate is measured.
    if not link.get("up") or not link.get("speed"):
        return {}
    return {"uplink": link["name"], "if_table": [dict(link, num_port=1)]}
