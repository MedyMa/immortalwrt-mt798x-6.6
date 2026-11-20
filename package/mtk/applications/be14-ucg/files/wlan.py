"""Translate the locally bound UniFi WLAN to independent BE14 test BSSes."""
import re

BANDS = {"wifi0": "ng", "wifi1": "na", "wifi2": "6e"}
FIELDS = ("ssid", "encryption", "key", "ieee80211w", "hidden", "isolate")
TRUE = (True, 1, "1", "enabled", "true")
FALSE = (False, 0, "0", "disabled", "false")


def flag(value):
    if value in TRUE:
        return "1"
    if value in FALSE:
        return "0"
    raise ValueError("invalid WLAN boolean")


def flat(text):
    if not isinstance(text, str):
        raise ValueError("invalid controller settings")
    values = {}
    for line in text.split("\n"):
        key, sep, value = line.rstrip("\r").partition("=")
        if sep and key and not key.startswith("#"):
            if key in values and values[key] != value:
                raise ValueError("ambiguous controller setting")
            values[key] = value
    return values


def groups(values, prefix):
    result = {}
    for key, value in values.items():
        parts = key.split(".", 2)
        if len(parts) == 3 and parts[0] == prefix and parts[1].isdigit():
            result.setdefault(parts[1], {})[parts[2]] = value
    return result


def valid_wlan_id(value):
    # Network can provision a site's numeric WLAN number or its ObjectId.
    return isinstance(value, str) and re.fullmatch(r"(?:[0-9]{1,10}|[0-9a-fA-F]{24})", value) is not None


def report_wlans(msg):
    if "system_cfg" in msg:
        rows = groups(flat(msg["system_cfg"]), "wireless").values()
    else:
        rows = msg["vap_table"]
        if not isinstance(rows, list):
            raise ValueError("invalid controller VAP table")
    by_band = {}
    for row in rows:
        if not isinstance(row, dict):
            continue
        band = row.get("radio") or BANDS.get(row.get("parent") or row.get("radio_name"))
        ssid = row.get("ssid") or row.get("essid")
        ident = str(row.get("id", ""))
        if (band not in BANDS.values() or not isinstance(ssid, str) or not 1 <= len(ssid.encode()) <= 32
                or not valid_wlan_id(ident) or row.get("usage", "user") != "user"
                or row.get("status") == "disabled" or row.get("enabled") in FALSE
                or row.get("is_guest") in TRUE or row.get("guest_policy") in TRUE):
            continue
        names = by_band.setdefault(band, {})
        # Distinct WLANs with an identical SSID on one band cannot be matched by name.
        if names.setdefault(ssid, ident) != ident:
            names[ssid] = None
    return by_band


def wlan_choices(mapping):
    choices = {}
    for band, names in (mapping or {}).items():
        for ssid, ident in names.items():
            if valid_wlan_id(ident):
                choices.setdefault((ident, ssid), []).append(band)
    return [dict(id=ident, ssid=ssid, bands=bands) for (ident, ssid), bands in sorted(choices.items())]


def automatic_choices(msg, mapping):
    result = []
    for choice in wlan_choices(mapping):
        ident = choice["id"]
        try:
            rows = sys_vaps(msg["system_cfg"], ident) if "system_cfg" in msg else msg["vap_table"]
            # Use the same policy checks for the picker and for the eventual push.
            offered = {b: dict(eligible=True, section=None, device=None) for b in choice["bands"]}
            plan(rows, dict(bindings=offered, automatic=True, management_network=None), ident)
        except ValueError:
            continue
        result.append(choice)
    return result


def sys_vaps(text, wlan_id, automatic=False):
    values = flat(text)
    auths = groups(values, "aaa")
    rows = []
    for i, vif in groups(values, "wireless").items():
        if vif.get("id") != wlan_id:
            continue
        if automatic and vif.get("status") == "disabled":
            continue
        # Match the per-BSS authenticator by its devname; controller indices can differ.
        dev = vif.get("devname")
        matches = [a for a in auths.values() if dev and a.get("devname") == dev]
        auth = matches[0] if len(matches) == 1 else auths.get(i, {})
        if len(matches) > 1 or (dev and auth.get("devname", dev) != dev) or auth.get("ssid", vif.get("ssid")) != vif.get("ssid"):
            raise ValueError("ambiguous WLAN authenticator")
        if (vif.get("usage", "user") != "user" or vif.get("status") == "disabled" or
                vif.get("is_guest") in TRUE or auth.get("is_guest") in TRUE or
                auth.get("wpa.psk_file.status") in TRUE or vif.get("vlan", "0") not in ("", "0") or
                any(v.get("devname") == dev and v.get("id", "0") != "0" for v in groups(values, "vlan").values()) or
                (auth.get("br.devname", "br0") != "br0")):
            raise ValueError("test WLAN requires an enabled untagged personal network")
        mgmt = {token for k, v in auth.items() if re.fullmatch(r"wpa\.key\.\d+\.mgmt", k) for token in v.split()}
        if mgmt == {"OWE"}:
            security = "owe"
        elif "SAE" in mgmt and mgmt <= {"SAE", "WPA-PSK"}:
            security = "sae-mixed" if auth.get("wpa3.transition") in TRUE or "WPA-PSK" in mgmt else "sae"
        elif mgmt == {"WPA-PSK"} and auth.get("wpa") == "2":
            security = "psk2"
        else:
            raise ValueError("unsupported WLAN authentication")
        rows.append(dict(id=wlan_id, radio=BANDS.get(vif.get("parent")), essid=vif.get("ssid"),
                         security=security, x_passphrase=auth.get("wpa.psk"),
                         hide_ssid=vif.get("hide_ssid"), isolate=vif.get("l2_isolation"),
                         pmf=auth.get("pmf.mode")))
    return rows


def plan(rows, snapshot, wlan_id, allow_withdraw=False):
    if not isinstance(rows, list):
        raise ValueError("invalid VAP table")
    changes, expected = {}, {}
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError("invalid VAP entry")
        if str(row.get("id", "")) != wlan_id:
            continue
        if allow_withdraw and (row.get("status") == "disabled" or row.get("enabled") in FALSE):
            continue
        code = row.get("radio") or BANDS.get(row.get("radio_name"))
        if code not in BANDS.values():
            raise ValueError("unknown radio for bound WLAN")
        binding = snapshot["bindings"].get(code)
        if not binding:
            continue
        if code in expected:
            raise ValueError("duplicate WLAN for band")
        ssid = row.get("essid")
        if not isinstance(ssid, str) or not 1 <= len(ssid.encode()) <= 32 or re.search(r"[\x00-\x1f\x7f]", ssid):
            raise ValueError("invalid WLAN SSID")
        if (row.get("usage", "user") != "user" or row.get("status") == "disabled"
                or any(flag(row.get(k, False)) == "1" for k in ("guest_policy", "is_guest", "vlan_enabled", "dynamic_vlan"))
                or any(row.get(k) not in (None, 0, "0", "") for k in ("vlan", "vlan_id"))
                or flag(row.get("enabled", True)) == "0"):
            raise ValueError("unsupported WLAN network policy")
        automatic = snapshot.get("automatic", False)
        if automatic and (not binding.get("eligible") or ssid in snapshot.get("other_ssids", {})
                          or row.get("security") is None):
            raise ValueError("automatic WLAN requires an available secondary AP, distinct SSID and explicit security")
        fields = FIELDS + (("disabled", "be14_ucg_managed", "network") if automatic else ())
        before = {k: binding[k] for k in fields if binding.get(k) is not None}
        after = dict(before, ssid=ssid)
        if automatic:
            after.pop("disabled", None)
            after["be14_ucg_managed"] = "1"
            after["network"] = snapshot["management_network"]
        if row.get("security") is not None:
            mode = {"wpapsk": "psk2", "wpa2-psk": "psk2", "wpa3-sae": "sae",
                    "wpa2-wpa3": "sae-mixed"}.get(row["security"], row["security"])
            if mode not in ("psk2", "sae", "sae-mixed", "owe") or (code == "6e" and mode not in ("sae", "owe")):
                raise ValueError("unsupported WLAN encryption for band")
            after["encryption"] = mode
            if mode == "owe":
                after.pop("key", None)
            else:
                key = row.get("x_passphrase")
                if not isinstance(key, str) or re.search(r"[\x00-\x1f\x7f]", key) or not (
                        8 <= len(key.encode()) <= 63 or (mode == "psk2" and re.fullmatch(r"[0-9a-fA-F]{64}", key))):
                    raise ValueError("invalid WLAN passphrase")
                after["key"] = key
            pmf = row.get("pmf")
            after["ieee80211w"] = str(pmf if pmf is not None else {"sae": 2, "owe": 2, "sae-mixed": 1, "psk2": 0}[mode])
            if after["ieee80211w"] not in ("0", "1", "2") or (mode in ("sae", "owe") and after["ieee80211w"] != "2") or (mode == "sae-mixed" and after["ieee80211w"] == "0"):
                raise ValueError("invalid WLAN PMF policy")
        for wire, local in (("hide_ssid", "hidden"), ("isolate", "isolate")):
            if row.get(wire) is not None:
                after[local] = flag(row[wire])
            elif automatic:
                after[local] = "0"
        expected[code] = dict(section=binding["section"], device=binding["device"], settings=after)
        if before != after:
            changes[code] = dict(section=binding["section"], device=binding["device"], before=before, after=after)
            if binding.get("missing"):
                changes[code]["create"] = True
    if allow_withdraw:
        for code in snapshot["bindings"].keys() - expected.keys():
            b = snapshot["bindings"][code]
            if not (b.get("missing") or b.get("disabled") == "1" or b.get("managed_secondary")):
                raise ValueError("cannot withdraw a locally owned AP")
            expected[code] = dict(section=b["section"], device=b["device"], inactive=True)
            if b.get("be14_ucg_managed") == "1" and b.get("disabled") != "1":
                before = {k: b[k] for k in FIELDS + ("disabled", "be14_ucg_managed", "network") if k in b}
                changes[code] = dict(section=b["section"], device=b["device"], before=before, after=dict(before, disabled="1"))
    if set(expected) != set(snapshot["bindings"]):
        raise ValueError("controller has not provisioned every bound band")
    return changes, expected
