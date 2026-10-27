#!/usr/bin/env python3
"""Run the actual status script against fixture nodes with real host jshn.

Only fixed absolute input paths and the sourced jshn.sh path are relocated in a
temporary copy. No device access, kernel operations or shell JSON mocks.
The host jshn executable and jshn.sh must be supplied; all output stays external.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--jshn-bin', type=Path, required=True)
p.add_argument('--jshn-shell', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--revision', help='Optional Git revision for a failing baseline')
a = p.parse_args()
root = Path(__file__).resolve().parents[4]
path = 'package/utils/mtk-tops-accounting/files/mtk-tops-status'
text = subprocess.check_output(['git', '-C', str(root), 'show', a.revision + ':' + path], text=True) if a.revision else (root/path).read_text()
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
header = '# schema=1 max_tunnels=32 snapshot=per_entry unit=metadata\n'
state = {'schema': 1, 'firmware_ready': True, 'mcu_alive': True, 'registered_tunnel_types': 3}
gre = '0 18446744073709551615 9 1 47 192.0.2.1 0 198.51.100.1 0 1 1 1\n'
vxlan = '1 2 10 5 17 192.0.2.1 4789 198.51.100.2 4789 1 0 0\n'
l2tp = '2 3 11 3 17 192.0.2.1 1701 198.51.100.3 1701 0 1 1\n'
rows = []
with tempfile.TemporaryDirectory(prefix='tops-status-contract-') as temp:
    fixture = Path(temp)
    binary = fixture/'bin'; binary.mkdir(); (binary/'jshn').symlink_to(a.jshn_bin.resolve())
    nodes = fixture/'nodes'
    npu = nodes/'sys/bus/platform/drivers/mediatek,npu/15100000.npu/npu/mcu/state'
    tunnels = nodes/'proc/npu/tunnels'
    acct = nodes/'proc/sys/net/netfilter/nf_conntrack_acct'
    ledger = nodes/'sys/kernel/debug/hnat/accounting'
    for node in [npu, tunnels, acct, ledger]: node.parent.mkdir(parents=True, exist_ok=True)
    relocated = text.replace('/usr/share/libubox/jshn.sh', str(a.jshn_shell.resolve()))
    relocated = relocated.replace('/sys/', str(nodes) + '/sys/').replace('/proc/', str(nodes) + '/proc/')
    script = fixture/'status.sh'; script.write_text(relocated)

    def run(name, tunnel_text=header+gre, npu_text=None, expected_coverage='complete', expected_entries=1,
            ready=True, npu_coverage='complete', remove=None, no_timeout=False, expected_health=None):
        npu.write_text(json.dumps(state) if npu_text is None else npu_text)
        tunnels.write_text(tunnel_text); acct.write_text('1\n'); ledger.write_text('software ledger\n')
        if remove: remove.unlink()
        env = os.environ.copy(); env['PATH'] = str(binary) + ('' if no_timeout else ':' + env.get('PATH', ''))
        cmd = ['/bin/bash', str(script)]
        r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=8)
        errors = []
        try:
            d = json.loads(r.stdout)
            e = d['tunnels']['entries']
            if len(e) != expected_entries: errors.append('entry count')
            if d['tunnels']['coverage'] != expected_coverage: errors.append('tunnel coverage')
            if d['npu']['coverage'] != npu_coverage: errors.append('NPU coverage')
            if d['npu']['firmware_ready'] != ready: errors.append('firmware readiness')
            if d['npu']['packet_offload_verified'] is not False: errors.append('unproven hardware verification')
            if d['accounting']['add_tops_events_to_byte_totals'] is not False: errors.append('event/byte mixing')
            if 'count' in d['tunnels'] and d['tunnels']['count'] != len(e): errors.append('summary count')
            if 'synchronized_count' in d['tunnels'] and d['tunnels']['synchronized_count'] != sum(x['synchronized'] for x in e): errors.append('sync count')
            if name == 'all-protocols' and e and e[0]['generation'] != '18446744073709551615': errors.append('u64 precision')
            if not a.revision and d['schema'] != 3: errors.append('schema 3')
            if len(r.stdout.encode()) > 32768: errors.append('RPC output bound')
            if expected_health is not None and (not e or e[0].get('sync_health') != expected_health): errors.append('sync health')
        except (ValueError, KeyError, TypeError):
            errors.append('invalid JSON contract'); d = None
        if r.returncode: errors.append('script exit')
        if r.stderr: errors.append('unexpected stderr')
        row = {'case': name, 'exit': r.returncode, 'pass': not errors, 'errors': errors, 'stderr': r.stderr[:300], 'output': d}
        rows.append(row)

    run('all-protocols', header+gre+vxlan+l2tp, expected_entries=3)
    good_health = '# sync 0 18446744073709551615 2 1 6 0 18446744073709551615\n'
    run('sync-health-success', header+good_health+gre, expected_health={
        'unit':'parameter_sync_attempts', 'attempts':'2', 'failures':'1', 'stage':6,
        'last_error':0, 'synced_generation':'18446744073709551615'})
    run('sync-health-mgmt-error', header+'# sync 1 2 3 3 3 -110 0\n'+vxlan, expected_health={
        'unit':'parameter_sync_attempts', 'attempts':'3', 'failures':'3', 'stage':3,
        'last_error':-110, 'synced_generation':'0'})
    run('sync-health-generation-mismatch', header+good_health.replace(' 0 18446744073709551615 ', ' 0 3 ', 1)+gre,
        expected_coverage='invalid_entries')
    run('sync-health-invalid-stage', header+good_health.replace(' 6 0 ', ' 7 0 ')+gre,
        expected_coverage='invalid_entries')
    run('sync-health-invalid-counter', header+good_health.replace(' 2 1 ', ' bad 1 ')+gre,
        expected_coverage='invalid_entries')
    run('sync-health-orphan', header+good_health, expected_entries=0, expected_coverage='invalid_entries')
    worst=''.join('# sync %d 18446744073709551615 18446744073709551615 18446744073709551615 5 -4095 18446744073709551615\n%d 18446744073709551615 2147483647 1 47 255.255.255.255 0 255.255.255.255 0 1 1 0\n' % (i,i) for i in range(32))
    assert len(header+worst)<8192
    run('sync-health-full-capacity', header+worst, expected_entries=32)
    run('empty-table', header, expected_entries=0)
    run('missing-tunnel-node', remove=tunnels, expected_entries=0, expected_coverage='missing')
    run('missing-NPU-node', remove=npu, ready=False, npu_coverage='missing')
    run('invalid-NPU-json', npu_text='{', ready=False, npu_coverage='invalid_json')
    run('invalid-NPU-schema', npu_text=json.dumps(dict(state, schema=2)), ready=False, npu_coverage='invalid_schema')
    run('string-readiness', npu_text=json.dumps(dict(state, firmware_ready='true')), ready=False, npu_coverage='invalid_schema')
    run('invalid-type-count', npu_text=json.dumps(dict(state, registered_tunnel_types=33)), ready=False, npu_coverage='invalid_schema')
    run('unknown-tunnel-schema', '# schema=2\n'+gre, expected_entries=0, expected_coverage='unsupported_schema')
    run('empty-tunnel-file', '', expected_entries=0, expected_coverage='unsupported_schema')
    run('invalid-IPv4', header+gre.replace('192.0.2.1', '999.0.2.1'), expected_entries=0, expected_coverage='invalid_entries')
    run('trailing-dot-IPv4', header+gre.replace('192.0.2.1 ', '192.0.2.1. '), expected_entries=0, expected_coverage='invalid_entries')
    run('oversized-port', header+vxlan.replace(' 4789 ', ' 65536 ', 1), expected_entries=0, expected_coverage='invalid_entries')
    run('invalid-boolean', header+gre.replace(' 1 1 1\n', ' 1 1 2\n'), expected_entries=0, expected_coverage='invalid_entries')
    run('missing-boolean', header+gre.rsplit(' ', 1)[0]+'\n', expected_entries=0, expected_coverage='invalid_entries')
    run('duplicate-index', header+gre+gre, expected_entries=1, expected_coverage='invalid_entries')
    run('generation-overflow', header+gre.replace('18446744073709551615', '18446744073709551616'), expected_entries=0, expected_coverage='invalid_entries')
    run('generation-leading-zero', header+gre.replace('18446744073709551615', '01'), expected_entries=0, expected_coverage='invalid_entries')
    run('noncanonical-index', header+vxlan.replace('1 ', '01 ', 1), expected_entries=0, expected_coverage='invalid_entries')
    run('unknown-protocol', header+gre.replace(' 1 47 ', ' 9 47 '), expected_entries=0, expected_coverage='invalid_entries')
    run('partial-table', header+gre+vxlan.replace(' 4789 ', ' 65536 ', 1), expected_entries=1, expected_coverage='invalid_entries')
    run('newline-truncation', '\n'*8193, expected_entries=0, expected_coverage='truncated')
    large = json.dumps(dict(state, padding=''))
    large = json.dumps(dict(state, padding='x'*(1023-len(large))))
    assert len(large) == 1023
    run('json-newline-truncation', npu_text=large+'\n\n', ready=False, npu_coverage='truncated')
    run('no-timeout', no_timeout=True, expected_entries=0, expected_coverage='no_timeout_tool', ready=False, npu_coverage='no_timeout_tool')

receipt = {'revision': a.revision or 'working tree', 'source_sha256': hashlib.sha256(text.encode()).hexdigest(),
           'jshn_sha256': hashlib.sha256(a.jshn_bin.read_bytes()).hexdigest(),
           'jshn_shell_sha256': hashlib.sha256(a.jshn_shell.read_bytes()).hexdigest(),
           'scope': __doc__, 'cases': rows, 'passed': sum(r['pass'] for r in rows), 'failed': sum(not r['pass'] for r in rows)}
(out/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
print(json.dumps({'passed': receipt['passed'], 'failed': receipt['failed'], 'failures': [{'case':r['case'],'errors':r['errors']}for r in rows if not r['pass']]}))
raise SystemExit(bool(receipt['failed']))
