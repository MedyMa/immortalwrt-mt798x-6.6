#!/usr/bin/python3 -B
"""Optional DHCP endpoint for the router on its Ethernet uplink."""
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import select
import signal
import socket
import subprocess
import sys
import tempfile
import time

from wired import NET, read, unicast, wired_uplink

IFACE = "r4-wired"
STATUS = Path("/var/run/be14-ucg-wired.json")
INTERVAL = 30


def write(path, data):
    tmp = path.with_suffix(".new")
    tmp.write_text(json.dumps(data))
    tmp.replace(path)


def status():
    try:
        data = json.loads(STATUS.read_text())
        if 0 <= time.time() - data["updated"] < 3 * INTERVAL:
            return data
    except (OSError, KeyError, TypeError, ValueError):
        pass
    return {}


def parent_port(network, url):
    row = subprocess.check_output(["ubus", "call", "network.interface." + network, "status"], timeout=4)
    device = json.loads(row).get("l3_device")
    link = wired_uplink({"management_device": device, "url": url})
    if not link.get("up"):
        return None
    mac = read(NET / device / "address").lower()
    if unicast(mac):
        return device, mac


def host_mac(parent):
    raw = bytearray(hashlib.sha256(("be14-wired-host/" + parent).encode()).digest()[:6])
    raw[0] = (raw[0] & 0xfc) | 2
    return raw.hex(":")


def lease_event(event):
    # udhcpc invokes this only inside the isolated network namespace.
    path = Path(os.environ["BE14_HOST_LEASE"])
    if os.environ.get("interface") != IFACE:
        return
    prefix = ["ip", "-4"]
    if event == "deconfig":
        subprocess.run(prefix + ["addr", "flush", "dev", IFACE], check=True)
        path.unlink(missing_ok=True)
    elif event in ("bound", "renew"):
        addr = ipaddress.IPv4Interface(os.environ["ip"] + "/" + os.environ.get("subnet", "255.255.255.255"))
        previous = json.loads(path.read_text()) if path.exists() else {}
        if previous.get("cidr") != str(addr):
            subprocess.run(prefix + ["addr", "flush", "dev", IFACE], check=True)
        subprocess.run(prefix + ["addr", "replace", str(addr), "dev", IFACE], check=True)
        gateways = os.environ.get("router", "").split()
        gateway = str(ipaddress.IPv4Address(gateways[0])) if gateways else None
        if gateway:
            subprocess.run(prefix + ["route", "replace", gateway + "/32", "dev", IFACE], check=True)
        write(path, {"ip": str(addr.ip), "cidr": str(addr), "gateway": gateway})


def guest(directory, parent, mac):
    # This handshake precedes DHCP and lets the supervisor create the link
    # directly in this process's namespace, without naming it in /run/netns.
    print("ready", flush=True)
    if not sys.stdin.readline():
        return
    subprocess.run(["ip", "link", "set", IFACE, "up"], check=True)
    lease = Path(directory) / "lease.json"
    env = dict(os.environ, BE14_HOST_LEASE=str(lease))
    dhcp = subprocess.Popen(["udhcpc", "-f", "-R", "-i", IFACE, "-x", "hostname:BPI-R4", "-s", __file__],
                            env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL)
    try:
        for _ in sys.stdin:
            if dhcp.poll() is not None:
                break
            try:
                socket.if_nametoindex(IFACE)
            except OSError:
                break
            try:
                record = json.loads(lease.read_text())
            except FileNotFoundError:
                record = {}
            gateway = record.get("gateway")
            if gateway:
                result = subprocess.run(["ping", "-4", "-c", "1", "-W", "2", gateway], stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL, timeout=5)
                record["gateway_reachable"] = result.returncode == 0
            record.pop("cidr", None)
            write(STATUS, dict(record, state="leased" if record else "dhcp", parent=parent, mac=mac, updated=time.time()))
    finally:
        dhcp.terminate()
        dhcp.wait(timeout=8)


def serve(network, url, port):
    parent, original = port
    mac = host_mac(original)
    with tempfile.TemporaryDirectory(prefix="be14-wired-") as directory:
        child = subprocess.Popen(["unshare", "--net", sys.executable, "-B", __file__, "guest", directory, parent, mac],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            if (not select.select([child.stdout], [], [], 6)[0] or child.stdout.readline().strip() != "ready"
                    or os.readlink(f"/proc/{child.pid}/ns/net") == os.readlink("/proc/self/ns/net")):
                raise RuntimeError("isolated network unavailable")
            subprocess.run(["ip", "link", "add", "link", parent, "name", IFACE, "netns", str(child.pid),
                            "address", mac, "type", "macvlan", "mode", "private"], check=True, timeout=5)
            child.stdin.write("start\n")
            while child.poll() is None:
                child.stdin.write("tick\n")
                child.stdin.flush()
                time.sleep(INTERVAL)
                if parent_port(network, url) != port:
                    break
            else:
                raise RuntimeError("DHCP interface stopped")
        finally:
            try:
                child.stdin.close()
            except BrokenPipeError:
                pass
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.terminate()
                child.wait(timeout=5)
            child.stdout.close()


def monitor(network, url):
    if not re.fullmatch(r"[a-zA-Z0-9_]+", network):
        raise ValueError("invalid management network")
    try:
        while True:
            try:
                port = parent_port(network, url)
                if port:
                    serve(network, url, port)
                state = "waiting"
            except (OSError, ValueError, RuntimeError, subprocess.SubprocessError):
                state = "error"
            write(STATUS, {"state": state, "updated": time.time()})
            time.sleep(INTERVAL)
    finally:
        STATUS.unlink(missing_ok=True)


def stop(*_):
    raise SystemExit


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, stop)
    if sys.argv[1:2] == ["guest"]:
        guest(*sys.argv[2:])
    elif sys.argv[1:2] == ["monitor"]:
        monitor(*sys.argv[2:])
    else:
        lease_event(sys.argv[1])
