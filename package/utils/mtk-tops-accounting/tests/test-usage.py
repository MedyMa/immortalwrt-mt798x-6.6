#!/usr/bin/env python3
"""Exercise the actual bounded usage helper against offline proc fixtures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
source = Path(__file__).resolve().parents[1] / 'files/mtk-npu-usage'
text = source.read_text()
a.output.mkdir(parents=True, exist_ok=True)
rows = []
with tempfile.TemporaryDirectory(prefix='npu-usage-contract-') as temp:
    root = Path(temp)
    node = root / 'utilization'
    capability = root / 'capability'
    script = root / 'helper.sh'
    script.write_text(text.replace('/proc/npu/cpu-utilization', str(node)).replace('/proc/npu/telemetry-capabilities', str(capability)))
    empty_bin = root / 'empty-bin'
    empty_bin.mkdir()
    valid = 'CPU Utilization:\nCore0\t0%\nCore1\t10%\nCore2\t20%\nCore3\t30%\nCoreM\t40%\n'

    def check(name, data=valid, coverage='complete', milli=20000, missing=False, no_timeout=False, fifo=False, legacy=False):
        if node.exists():
            node.unlink()
        if capability.exists():
            capability.unlink()
        if not legacy:
            capability.write_text('schema=1 mailbox_bounded=1 utilization_cores=5\n')
        if fifo:
            os.mkfifo(node)
        elif not missing:
            node.write_text(data)
        env = os.environ.copy()
        if no_timeout:
            env['PATH'] = str(empty_bin)
        result = subprocess.run(['/bin/sh', str(script)], env=env, capture_output=True, text=True, timeout=5)
        response = json.loads(result.stdout)
        assert result.returncode == 0 and len(result.stdout) < 1024
        assert response['schema'] == 1 and response['unit'] == 'mean_core_busy_percent'
        assert response['coverage'] == coverage, (name, response)
        assert response['available'] == (coverage == 'complete'), name
        if coverage == 'complete':
            assert response['percent_milli'] == milli and response['core_count'] == 5, (name, response)
        else:
            assert 'percent_milli' not in response, name
        rows.append({'case': name, 'pass': True, 'response': response})

    check('five-native-core-values')
    check('zero-is-real-only-with-complete-sample', valid.replace('10%', '0%').replace('20%', '0%').replace('30%', '0%').replace('40%', '0%'), milli=0)
    check('all-busy', 'CPU Utilization:\n' + ''.join(f'Core{i}\t100%\n' for i in ['0','1','2','3','M']), milli=100000)
    check('no-final-newline', valid.rstrip('\n'))
    check('missing-provider', missing=True, coverage='missing')
    check('missing-timeout', no_timeout=True, coverage='no_timeout_tool')
    check('blocked-open-is-bounded', fifo=True, coverage='read_failed')
    check('legacy-driver-never-queried', fifo=True, legacy=True, coverage='driver_unsupported')
    for name, data in [
        ('empty', ''), ('missing-core', valid.replace('CoreM\t40%\n', '')),
        ('duplicate-core', valid.replace('CoreM', 'Core0')), ('unknown-core', valid.replace('CoreM', 'Core4')),
        ('wrong-header', valid.replace('CPU Utilization:', 'Ready:')),
        ('negative', valid.replace('10%', '-1%')), ('above-100', valid.replace('10%', '101%')),
        ('fraction', valid.replace('10%', '10.5%')), ('leading-zero', valid.replace('10%', '010%')),
        ('missing-percent', valid.replace('10%', '10')), ('extra-field', valid.replace('10%', '10% ready')),
        ('extra-line', valid + 'Ready\n'), ('only-state', 'firmware ready\n'),
        ('extra-blank-line', valid + '\n'), ('shell-text-is-data', valid.replace('10%', '$(false)')),
    ]:
        check(name, data, coverage='invalid')
    check('oversized-input', valid + 'x' * 600, coverage='oversized')
    check('truncated-newlines-not-stripped', valid + '\n' * 600, coverage='oversized')

receipt = {'scope': __doc__, 'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'passed': len(rows), 'failed': 0, 'cases': rows}
(a.output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps({'passed': len(rows), 'failed': 0}))
