#!/usr/bin/env python3
"""Exercise the native OpenWrt configuration graph, including diagnostics.

Run against a prepared metadata/configuration view, not a device. A zero make
exit code is insufficient: older conf versions can report dependency errors
and still return success. Use the native build's GNU awk in PATH. Restore the
original configuration after every run.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def sha(data):
    return hashlib.sha256(data).hexdigest()


def values(data):
    return dict(re.findall(r"^(CONFIG_[\w-]+)=(.*)$", data.decode(), re.M))


def seed(data, changes):
    keys = set(changes)
    lines = [line for line in data.decode().splitlines()
             if not any(line.startswith(k + "=") or
                        line == "# " + k + " is not set" for k in keys)]
    for key, val in changes.items():
        if val is not None:
            lines.append("# " + key + " is not set" if val == "n"
                         else key + "=" + val)
    return ("\n".join(lines) + "\n").encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--topdir", type=Path, required=True)
    parser.add_argument("--defconfig", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    awk_argv = ["awk", "BEGIN { a[1] = 1; asort(a); exit(a[1] != 1) }"]
    awk_check = subprocess.run(awk_argv, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, timeout=10)
    if awk_check.returncode:
        parser.error("GNU awk with asort is required; use the native host tools in PATH")
    top = args.topdir.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    originals = {name: (top / name).read_bytes() if (top / name).exists()
                 else None for name in (".config", ".config.old")}
    release = args.defconfig.read_bytes()
    backend = "CONFIG_MTK_NPU_24_10_TUNNEL_BACKEND"
    cases = [
        ("profile", {}, {backend: "y"}),
        ("implicit_default", {backend: None}, {backend: "y"}),
        ("explicit_backend_off", {backend: "n"}, {
            backend: "n", "CONFIG_MTK_NPU_HNAT": "n",
            "CONFIG_PACKAGE_kmod-npu-nf_hnat": "n"}),
        ("protocol_optout", {
            "CONFIG_MTK_NPU_GRETAP": "n", "CONFIG_MTK_NPU_VXLAN": "n",
            "CONFIG_MTK_NPU_L2TP_V2": "n"}, {
            "CONFIG_MTK_NPU_GRETAP": "n", "CONFIG_MTK_NPU_VXLAN": "n",
            "CONFIG_MTK_NPU_L2TP_V2": "n"}),
    ]
    rows = []
    try:
        for name, changes, expect in cases:
            (top / ".config").write_bytes(seed(release, changes))
            argv = ["make", "-C", str(top), "-j2", "defconfig", "V=s"]
            proc = subprocess.run(argv, env=os.environ.copy(),
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, timeout=120)
            log = args.output / (name + ".log")
            log.write_bytes(proc.stdout)
            output = (top / ".config").read_bytes()
            (args.output / (name + ".config")).write_bytes(output)
            config = values(output)
            errors = [line for line in proc.stdout.decode(errors="replace").splitlines()
                      if re.search(r"error:|recursive dependency detected", line)]
            expect["CONFIG_TARGET_ROOTFS_PARTSIZE"] = "8192"
            mismatch = {k: {"expected": v, "actual": config.get(k, "n")}
                        for k, v in expect.items() if config.get(k, "n") != v}
            rows.append({"case": name, "argv": argv, "exit": proc.returncode,
                         "errors": errors, "mismatches": mismatch,
                         "expected": expect, "config_sha256": sha(output),
                         "log": str(log), "log_sha256": sha(proc.stdout)})
            assert proc.returncode == 0 and not errors and not mismatch, rows[-1]
        assert rows[0]["config_sha256"] == rows[1]["config_sha256"]
    finally:
        for name, data in originals.items():
            if data is None:
                (top / name).unlink(missing_ok=True)
            else:
                (top / name).write_bytes(data)
        receipt = {"scope": "native configuration, no hardware boot claim",
                   "awk_preflight": {"argv": awk_argv, "exit": awk_check.returncode},
                   "defconfig_sha256": sha(release), "cases": rows,
                   "restored_config_sha256": sha((top / ".config").read_bytes())}
        (args.output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print("PASS native configuration: profile/default/backend opt-out/protocol opt-out; no dependency errors")


if __name__ == "__main__":
    main()
