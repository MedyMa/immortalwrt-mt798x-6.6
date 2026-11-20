"""Device-side UniFi adoption, inform and BE14 WLAN provisioning."""
import contextlib
import fcntl
import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time
import zlib
from urllib.parse import urlsplit

from cryptography.exceptions import InvalidTag
from inform import FACTORY_KEY, HEADER, LIMIT, packet, unpack
from telemetry import wireless_report
from wired import uplink_report
from wlan import flat, plan, report_wlans, sys_vaps, valid_wlan_id

STATE = Path("/etc/be14-ucg/state.json")
LOCK = "/var/run/be14-ucg.lock"
BRIDGE = "/usr/lib/be14-ucg/wireless.lua"
MODEL = "E7"
DEFAULT_VERSION = "8.8.14.20179"
LAST_INFORM = Path("/var/run/be14-ucg.last-inform")


def firmware_version(value):
    if value in (None, ""):
        return DEFAULT_VERSION
    if not isinstance(value, str) or not re.fullmatch(r"(?:0|[1-9][0-9]{0,5})(?:\.(?:0|[1-9][0-9]{0,5})){2,3}", value):
        raise ValueError("invalid reported firmware version")
    return value


def endpoint(url):
    parts = urlsplit(url)
    if (parts.scheme not in ("http", "https") or parts.path != "/inform" or parts.query or
            parts.fragment or parts.username is not None or parts.password is not None):
        raise ValueError("configure a controller IP and /inform URL")
    ipaddress.ip_address(parts.hostname)
    return parts


def run(argv, **kwargs):
    return subprocess.run(argv, check=True, capture_output=True, timeout=15, **kwargs).stdout


def local(op, **data):
    with tempfile.TemporaryDirectory(prefix="be14-uci-") as private:
        return json.loads(run([BRIDGE, private], input=json.dumps(dict(op=op, **data)).encode()))


@contextlib.contextmanager
def locked():
    with open(LOCK, "a") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        yield


def save(state):
    STATE.parent.mkdir(mode=0o700, exist_ok=True)
    fd, name = tempfile.mkstemp(dir=STATE.parent)
    try:
        with os.fdopen(fd, "w") as file:
            json.dump(state, file, ensure_ascii=True)
            file.flush()
            os.fsync(file.fileno())
        os.replace(name, STATE)
        directory = os.open(STATE.parent, os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def state_for(url):
    endpoint(url)
    if not STATE.exists():
        return dict(url=url, key=FACTORY_KEY, gcm=False, cfgversion="0")
    state = json.loads(STATE.read_text())
    if state["url"] != url or not re.fullmatch(r"[0-9a-f]{32}", state["key"]):
        raise ValueError("saved adoption belongs to another controller or is invalid")
    return state


def new_key(key):
    if not isinstance(key, str) or not re.fullmatch(r"[0-9a-fA-F]{32}", key) or key.lower() == FACTORY_KEY:
        raise ValueError("invalid adoption key")
    return key.lower()


def post(url, data):
    target = endpoint(url)
    client = (http.client.HTTPSConnection if target.scheme == "https" else http.client.HTTPConnection)(
        target.hostname, target.port, timeout=10)
    try:
        client.request("POST", "/inform", data, {"Content-Type": "application/x-binary"})
        response = client.getresponse()
        if response.status == 404:
            return None
        if response.status != 200:
            raise OSError("inform rejected")
        return response.read(LIMIT + HEADER.size + 1)
    finally:
        client.close()


def report(snapshot, state):
    mac = snapshot["mac"].lower()
    if not re.fullmatch(r"(?:[0-9a-f]{2}:){5}[0-9a-f]{2}", mac):
        raise ValueError("invalid management MAC")
    ipaddress.IPv4Address(snapshot["ip"])
    adopted = state["key"] != FACTORY_KEY
    # The E7 inform shortname and board ID resolve to UAPA697 in the UniFi catalog.
    msg = dict(type="uap", model=MODEL, sysid=0xA697, model_display="BPI-R4 BE14", version=firmware_version(snapshot.get("firmware_version")),
               hostname=socket.gethostname(), serial=mac.replace(":", "").upper(), mac=mac,
               ip=snapshot["ip"], inform_url=state["url"], x_authkey=state["key"],
               default=not adopted, _default_key=not adopted, state=4 if adopted else 1,
               fw_caps=3, support_wifi6e="6e" in snapshot["radios"],
               sshd_port=snapshot["sshd_port"], cfgversion=state["cfgversion"],
               uptime=int(float(Path("/proc/uptime").read_text().split()[0])), time=int(time.time()))
    if adopted:
        msg.update(wireless_report(snapshot, state.get("report_wlans", {})))
        msg.update(uplink_report(snapshot))
        # An adopted U7 record may have acknowledged a WLAN configuration
        # that omitted 6 GHz. Request a fresh one with the E7 capabilities.
        if state.get("wlan_model") != MODEL and (state["gcm"] or state["url"].startswith("https://")):
            msg["cfgversion"] = "0"
    return msg


def reload_wifi():
    run(["ubus", "call", "network", "reload"])


def revision(value):
    if not isinstance(value, str) or not re.fullmatch(r"[a-zA-Z0-9_.:-]{1,128}", value):
        raise ValueError("invalid configuration revision")
    return value


def active(bindings, expected):
    if set(bindings) != set(expected):
        return False
    for band, target in expected.items():
        live = bindings[band]
        settings = target["settings"]
        if (live.get("section") != target["section"] or live.get("device") != target["device"] or
                live.get("observed") != settings["ssid"] or
                any(live.get(k) != v or live.get("applied", {}).get(k) != v for k, v in settings.items())):
            return False
    return True


def finish_pending(state, read=None, reload=None, wait=time.sleep):
    read = read or local
    reload = reload or reload_wifi
    pending = state["pending"]
    changes = pending["changes"]
    def matches(binding, change, which):
        return (binding.get("section") == change["section"] and binding.get("device") == change["device"]
                and all(binding.get(k) == change[which].get(k) for k in change["before"].keys() | change["after"].keys()))
    snap = read("snapshot")
    bindings = snap["bindings"]
    if set(bindings) != set(pending["expected"]) or snap["wlan_id"] != pending["wlan_id"]:
        raise ValueError("WLAN binding changed while provisioning")
    def rollback():
        if changes:
            current = read("snapshot")["bindings"]
            if all(matches(current.get(c, {}), ch, "after") for c, ch in changes.items()):
                reverse = {c: dict(ch, before=ch["after"], after=ch["before"]) for c, ch in changes.items()}
                read("replace", changes=reverse)
            elif not all(matches(current.get(c, {}), ch, "before") for c, ch in changes.items()):
                raise ValueError("local edit prevents automatic rollback")
            reload()
        del state["pending"]
        save(state)

    if pending.get("rollback"):
        rollback()
        return

    # Resume safely on either side of UCI's atomic file replacement.
    if changes and all(matches(bindings[c], ch, "before") for c, ch in changes.items()):
        read("replace", changes=changes)
    elif not all(matches(bindings[c], ch, "after") for c, ch in changes.items()):
        raise ValueError("concurrent local WLAN edit, pending revision withheld")
    try:
        if changes:
            reload()
        for attempt in range(21):
            fresh = read("snapshot")
            if fresh["wlan_id"] == pending["wlan_id"] and active(fresh["bindings"], pending["expected"]):
                state["cfgversion"] = pending["version"]
                del state["pending"]
                save(state)
                return
            if attempt < 20:
                wait(1)
    except (OSError, ValueError, subprocess.SubprocessError):
        if "pending" not in state:
            raise
    pending["rollback"] = True
    save(state)
    rollback()
    raise ValueError("WLAN did not become active, revision withheld")


def handle(msg, state, snap, authenticated=False):
    typ = msg.get("_type")
    if typ == "noop":
        interval = msg.get("interval", 10)
        return max(3, min(60, interval)) if type(interval) == int else 10
    mgmt = flat(msg.get("mgmt_cfg", "")) if typ == "setparam" else {}
    adopt = typ == "cmd" and msg.get("cmd") in ("set-adopt", "adopt")
    setting = msg.get("key") if adopt else mgmt.get("authkey")
    if setting and (state["key"] == FACTORY_KEY or (adopt and authenticated)):
        if adopt and msg.get("uri") and msg["uri"] != state["url"]:
            raise ValueError("adoption URL differs from the configured controller")
        state.update(key=new_key(setting), gcm=state["gcm"] or mgmt.get("use_aes_gcm") in ("true", "1"))
        if not snap["wlan_id"] and not snap["bindings"] and mgmt.get("cfgversion"):
            state["cfgversion"] = state["mgmt_version"] = revision(mgmt["cfgversion"])
        save(state)
        # Provisioning starts with a response encrypted with the per-device key.
        return 1
    if state["key"] == FACTORY_KEY or typ not in ("setparam", "setstate"):
        return 10
    if mgmt.get("use_aes_gcm") in ("true", "1") and not state["gcm"]:
        state["gcm"] = True
        save(state)
    version = revision(msg.get("cfgversion") or mgmt.get("cfgversion") or state["cfgversion"])
    wlan_id = snap["wlan_id"]
    if "system_cfg" not in msg and "vap_table" not in msg:
        if "cfgversion" in mgmt and version != state.get("mgmt_version"):
            state["mgmt_version"] = version
            if not wlan_id:
                state["cfgversion"] = version
            save(state)
        return 5
    if bool(wlan_id) != bool(snap["bindings"]):
        raise ValueError("configure the WLAN ID and its local sections together")
    if wlan_id:
        if not authenticated:
            raise ValueError("WLAN provisioning requires GCM or verified HTTPS")
        if not valid_wlan_id(wlan_id):
            raise ValueError("invalid WLAN ID")
        rows = sys_vaps(msg["system_cfg"], wlan_id) if "system_cfg" in msg else msg["vap_table"]
        changes, expected = plan(rows, snap, wlan_id)
    else:
        changes, expected = {}, {}
    learned = report_wlans(msg) if authenticated else state.get("report_wlans")
    mapping_changed = learned != state.get("report_wlans")
    if mapping_changed:
        state["report_wlans"] = learned
    model_changed = authenticated and state.get("wlan_model") != MODEL
    if model_changed:
        state["wlan_model"] = MODEL
    if not changes:
        if expected and not active(snap["bindings"], expected):
            raise ValueError("WLAN live settings differ, revision withheld")
        if state["cfgversion"] != version or mapping_changed or model_changed:
            state["cfgversion"] = version
            save(state)
        return 5
    state["pending"] = dict(changes=changes, expected=expected, version=version, wlan_id=wlan_id)
    save(state)
    finish_pending(state)
    return 1


def tick():
    snap = local("snapshot")
    state = state_for(snap["url"])
    if "pending" in state:
        try:
            finish_pending(state)
        except (OSError, ValueError, subprocess.SubprocessError):
            print("Pending WLAN revision could not be confirmed", file=sys.stderr, flush=True)
        snap = local("snapshot")
    payload = report(snap, state)
    mac = bytes.fromhex(payload["mac"].replace(":", ""))
    reply = post(state["url"], packet(payload, mac, state["key"], state["gcm"]))
    if reply is None:
        return 5 if state["key"] == FACTORY_KEY else 15
    msg = unpack(reply, mac, state["key"], state["gcm"])
    if state["key"] != FACTORY_KEY:
        # tmpfs only. A saved adoption can outlive a reachable controller.
        try:
            with tempfile.NamedTemporaryFile(dir=LAST_INFORM.parent, delete=False, mode="w") as marker:
                json.dump([state["url"], payload["mac"], payload["version"], int(time.time())], marker)
            os.replace(marker.name, LAST_INFORM)
        except OSError:
            pass
    # An authenticated GCM reply can itself negotiate the switch from CBC.
    if int.from_bytes(reply[14:16], "big") & 8 and state["key"] != FACTORY_KEY and not state["gcm"]:
        state["gcm"] = True
        save(state)
    if "pending" in state:
        return 10
    protected = bool(int.from_bytes(reply[14:16], "big") & 8) or endpoint(state["url"]).scheme == "https"
    return handle(msg, state, snap, protected)


def main():
    os.umask(0o077)
    if len(sys.argv) != 1:
        if len(sys.argv) != 4 or sys.argv[1] != "set-adopt":
            return 1
        with locked():
            state = state_for(local("settings")["url"])
            if sys.argv[2] != state["url"]:
                return 1
            key = new_key(sys.argv[3])
            if state["key"] not in (FACTORY_KEY, key):
                return 1
            state["key"] = key
            save(state)
        return 0
    while True:
        try:
            with locked():
                interval = tick()
        except (OSError, ValueError, KeyError, TypeError, RecursionError, InvalidTag, zlib.error,
                http.client.HTTPException, subprocess.SubprocessError) as exc:
            # Controller replies and local UCI documents may contain credentials.
            print("BE14 inform failed (" + type(exc).__name__ + ")", file=sys.stderr, flush=True)
            interval = 10
        time.sleep(interval)


if __name__ == "__main__":
    sys.exit(main())
