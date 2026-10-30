#!/usr/bin/env python3
"""Exercise installed telemetry helpers with real host jshn and file fixtures."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--jshn-bin', type=Path, required=True)
p.add_argument('--jshn-shell', type=Path, required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[2]
source = repo/'package/utils/mtk-tops-accounting/files'
checks = 0

def check(value, label):
    global checks
    checks += 1
    assert value, label

with tempfile.TemporaryDirectory(prefix='mtk-telemetry-') as temp:
    t = Path(temp)
    binary = t/'bin'; binary.mkdir()
    (binary/'jshn').symlink_to(a.jshn_bin.resolve())
    env = dict(os.environ, PATH=str(binary)+':'+os.environ.get('PATH',''))
    paths = ['/proc/npu/telemetry-capabilities', '/proc/npu/cpu-utilization',
             '/proc/npu/tunnels', '/sys/kernel/debug/hnat/accounting',
             '/proc/sys/net/netfilter/nf_conntrack_acct', '/proc/device-tree/model',
             '/sys/bus/platform/drivers/mediatek,npu/*/npu/mcu/state',
             '/sys/bus/platform/drivers/mediatek,npu/*/npu/mcu/fw_info']
    local = {path:t/('node-'+str(i)) for i,path in enumerate(paths)}
    usage = binary/'mtk-npu-usage'
    text = (source/'mtk-npu-usage').read_text()
    for path, fixture in local.items(): text = text.replace(path,str(fixture))
    usage.write_text(text); usage.chmod(0o755)
    local[paths[0]].write_text('schema=1 mailbox_bounded=1 utilization_cores=5\n')
    report = 'CPU Utilization:\nCore0\t0%\nCore1\t20%\nCore2\t40%\nCore3\t60%\nCoreM\t80%\n'

    def run(script, shell='/bin/sh'):
        r = subprocess.run([shell,str(script)],env=env,capture_output=True,text=True,check=True,timeout=10)
        check(not r.stderr, 'helper does not print while sampling: '+r.stderr)
        return json.loads(r.stdout)

    local[paths[1]].write_text(report)
    value = run(usage)
    check(value['percent_milli']==40000 and value['cores']==dict(Core0=0,Core1=20000,Core2=40000,Core3=60000,CoreM=80000), 'five actual cores and their average')
    for bad in [report.replace('CoreM','Core3'),report.replace('80%','101%'),report.replace('0%\n','00%\n',1),report+'CoreM\t1%\n',report.replace('CoreM\t80%\n','')]:
        local[paths[1]].write_text(bad)
        check(run(usage)['available'] is False, 'malformed or partial core reports are unavailable')
    local[paths[0]].write_text('schema=1 mailbox_bounded=0 utilization_cores=5\n')
    check(run(usage)['coverage']=='driver_unsupported', 'unbounded mailbox provider is not polled')
    local[paths[0]].write_text('schema=1 mailbox_bounded=1 utilization_cores=5\n')
    local[paths[1]].write_text(report)
    local[paths[2]].write_text('# schema=1 max_tunnels=32 snapshot=per_entry unit=metadata\n')
    local[paths[3]].write_text('schema=1 unit=ppe_bytes_and_packets\nppe=0 final_reads=5 final_errors=0\n')
    local[paths[4]].write_text('1\n')
    local[paths[5]].write_bytes(b'Banana Pi BPI-R4\0')
    local[paths[6]].write_text(json.dumps(dict(schema=1,firmware_ready=True,mcu_alive=True,registered_tunnel_types=3)))
    firmware='MGMT version: test-mgmt\nOffload version: test-offload\n'
    local[paths[7]].write_text(firmware)
    status = t/'mtk-tops-status'
    text = (source/'mtk-tops-status').read_text().replace('/usr/share/libubox/jshn.sh',str(a.jshn_shell.resolve())).replace('/usr/bin/mtk-npu-usage',str(usage))
    for path, fixture in local.items(): text = text.replace(path,str(fixture))
    status.write_text(text)
    value = run(status, '/bin/bash')
    check(value['npu']['utilization']['cores']['CoreM']==80000, 'the status provider preserves core data')
    check(value['platform']=='Banana Pi BPI-R4' and value['npu']['firmware_info']==firmware.rstrip('\n'), 'platform and firmware come from actual driver nodes')
    check(not value['npu']['packet_offload_verified'] and not value['accounting']['lossless_hardware_tail_proven'], 'readiness and busy time do not assert hardware throughput')
    local[paths[6]].write_text(json.dumps(dict(schema=1,firmware_ready=False,mcu_alive=False,registered_tunnel_types=0)))
    value = run(status, '/bin/bash')
    check(not value['npu']['utilization']['available'] and value['npu']['firmware_info']=='', 'an offline NPU never fabricates a zero utilization or firmware version')
print(json.dumps(dict(checks=checks, failures=0, scope='Real helper parsers; synthetic sysfs/procfs only, no mailbox or hardware.')))
