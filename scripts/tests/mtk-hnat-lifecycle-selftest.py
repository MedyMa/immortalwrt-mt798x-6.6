#!/usr/bin/env python3
"""Execute production HNAT lifecycle functions with modeled MMIO and RCU.

This checks software retirement and publication, never WiFi reachability or
real PPE/DMA ordering. No router, debugfs mount or counter reset is accessed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source-tree', type=Path, default=Path(__file__).resolve().parents[2])
p.add_argument('--output', type=Path)
a = p.parse_args()
root = a.source_tree.resolve()
base = root/'target/linux/mediatek/files-6.6/drivers/net/ethernet/mediatek/mtk_hnat'
account = (base/'hnat_account.c').read_text()
hook = (base/'hnat_nf_hook.c').read_text()
header = (base/'hnat.h').read_text()
hnat_driver_path = base/'hnat.c'
hnat_driver = hnat_driver_path.read_text()
npu_base = root/'package/mtk/drivers/mtk_npu/src'
npu_event_path = npu_base/'npu-nf_hnat/net-event.c'
npu_core_path = npu_base/'tnl_offload.c'
npu_hook_path = npu_base/'npu-nf_hnat/tnl-offload.c'
npu_event = npu_event_path.read_text()
npu_core = npu_core_path.read_text()
npu_hook = npu_hook_path.read_text()

def function(source, name):
    match = re.search(r'(?m)^(?:static )?(?:inline )?(?:bool|int|void|u64)\s+'+name+r'\(', source)
    if not match:
        raise ValueError('missing production function: '+name)
    start = source.index('{',match.start()); end = start+1; depth = 1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}'); end += 1
    return source[match.start():end]

def structure(source, name):
    match = re.search(r'(?m)^struct '+name+r' \{', source)
    if not match:
        raise ValueError('missing production structure: '+name)
    return source[match.start():source.index('\n};',match.start())+3]

types = '\n'.join(structure(account,n) for n in ['hnat_owner','hnat_slot','hnat_account_ppe','hnat_account_ctx'])
types += '\n'+structure(header,'hnat_flow_entry')
names = ['hnat_account_enabled','account_valid','account_identity','owner_free',
         'flush_ct_locked','retire_locked','owner_matches','owner_live','drain_locked',
         'drain_final_locked','hnat_account_generation_snapshot','hnat_account_clear_snapshot',
         'account_commit','hnat_account_commit','account_clear','hnat_account_clear',
         'hnat_account_clear_if_generation','hnat_account_pause','hnat_account_reset_table',
         'hnat_account_resume','hnat_account_tunnel_invalidate']
names.append('hnat_account_invalidate')
functions = '\n'.join(function(account,n) for n in names)
functions += '\n'+'\n'.join(function(hook,n) for n in ['hnat_flow_entry_free_rcu',
    'hnat_flow_entry_delete','hnat_flow_entry_clear','hnat_flow_entry_store'])
if 'bool hnat_account_clear_all_bindings(' in account:
    functions += '\n'+function(account,'hnat_account_clear_all_bindings')
functions += '\n'+function(hook,'foe_clear_all_bind_entries_body')
fixture = Path(__file__).with_name('mtk-hnat-lifecycle-fixture.c').read_text()
code = fixture.replace('/* @PRODUCTION_TYPES@ */',types).replace('/* @PRODUCTION_FUNCTIONS@ */',functions)
if 'bool clearing;' in account:
    code = '#define TEST_HAS_CLEARING 1\n'+code

with tempfile.TemporaryDirectory(prefix='hnat-lifecycle-') as temp:
    c = Path(temp)/'test.c'; binary = Path(temp)/'test'
    c.write_text(code)
    subprocess.run(['cc','-std=gnu11','-O1','-g','-Wall','-Werror','-Wno-unused-function',
                    '-Wno-unused-variable','-Wno-sign-compare','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-I'+str(base),str(c),'-o',str(binary)],check=True)
    result = subprocess.run([str(binary)],capture_output=True,text=True)
    print(result.stdout,end='')
    if result.stderr: print(result.stderr,end='')
    static_checks = []

    going_down = function(hook, 'nf_hnat_netdevice_event')
    static_checks.append((
        'NETDEV_GOING_DOWN invokes the bulk HNAT clear',
        'case NETDEV_GOING_DOWN:' in going_down and 'foe_clear_all_bind_entries();' in going_down))

    going_up = going_down
    static_checks.append((
        'NETDEV_UP restores per-device PPE forwarding',
        'case NETDEV_UP:' in going_up and 'gmac_ppe_fwd_enable(dev);' in going_up and
        'extif_set_dev(dev,1);' in going_up))

    warm_init = function(hnat_driver, '__hnat_warm_init')
    enable_hook = function(hnat_driver, '__hnat_enable_hook')
    static_checks.append((
        'FE NAT reset warm-initializes PPE and resumes accounting publishers',
        'hnat_account_reset_table(hnat_priv, ppe_id)' in warm_init and
        'hnat_hw_init(ppe_id)' in warm_init and
        'if (was_enabled)\n\t\terr = __hnat_enable_hook();' in warm_init and
        'hnat_account_resume(hnat_priv, i)' in enable_hook))

    hnat_notifier = function(hook, 'nf_hnat_netdevice_event')
    static_checks.append((
        'MTK_FE_RESET_NAT_DONE dispatches HNAT warm init',
        'case MTK_FE_RESET_NAT_DONE:' in hnat_notifier and
        'hnat_warm_init();' in hnat_notifier))

    ser_callback = function(npu_event, 'mtk_npu_nf_hnat_netdev_ser_callback')
    ser_start = function(npu_event, 'mtk_npu_nf_hnat_netdev_ser')
    static_checks.append((
        'NPU SER waits for this FE reset completion and resets completion per cycle',
        'wait_for_completion_timeout(&wait_fe_reset_done' in ser_callback and
        'reinit_completion(&wait_fe_reset_done)' in ser_start and
        'call_netdevice_notifiers(MTK_TOPS_DUMP_DONE, netdev)' in ser_callback))

    reset_notifier = function(npu_event, 'mtk_npu_nf_hnat_netdev_callback')
    static_checks.append((
        'FE reset completion events wake the reset waiter',
        'case MTK_FE_RESET_DONE:' in reset_notifier and
        'case MTK_FE_START_TRAFFIC:' in reset_notifier and
        'complete(&wait_fe_reset_done);' in reset_notifier))

    npu_notifier = function(npu_event, 'mtk_npu_nf_hnat_netdev_callback')
    static_checks.append((
        'NETDEV_DOWN invalidates NPU tunnel templates',
        'case NETDEV_DOWN:' in npu_notifier and
        'mtk_npu_tnl_info_invalidate_netdev(dev);' in npu_notifier))

    invalidate = function(npu_core, 'tnl_info_invalidate')
    generation_at = invalidate.find('++info->generation')
    delete_at = invalidate.find('mtk_npu_tnl_info_delete_no_lock(info)')
    flush_at = invalidate.find('flush_index(i)')
    unlock_at = invalidate.find('spin_unlock_bh(&npu_tnl.tbl_lock)', delete_at)
    static_checks.append((
        'NPU invalidation advances generation and flushes HNAT after dropping table locks',
        0 <= generation_at < delete_at < unlock_at < flush_at))

    sync_delete = function(npu_core, 'mtk_npu_tnl_sync_param_delete')
    flush_at = sync_delete.find('tnl_info->ops->flush(tnl_info)')
    firmware_at = sync_delete.find('__mtk_npu_tnl_sync_param_delete(tnl_info)')
    error_at = sync_delete.find('if (ret)', firmware_at)
    free_at = sync_delete.find('mtk_npu_tnl_info_free(tnl_info)', firmware_at)
    static_checks.append((
        'NPU deletion flushes the HNAT owner before firmware deletion and preserves refs on error',
        0 <= flush_at < firmware_at < error_at < free_at and
        'return ret;' in sync_delete[error_at:free_at]))

    put_devices = function(npu_core, 'mtk_npu_tnl_info_put_devices')
    free_info = function(npu_core, 'mtk_npu_tnl_info_free_no_lock')
    static_checks.append((
        'NPU tunnel retirement drops device, underlay and egress refs',
        all(f'dev_put(tnl_info->{field})' in put_devices
            for field in ['dev', 'underlay', 'egress']) and
        'mtk_npu_tnl_info_put_devices(tnl_info)' in free_info))
    for label, passed in static_checks:
        print(f'{"PASS" if passed else "FAIL"} {label}')
    static_failures = sum(not passed for _, passed in static_checks)
    exit_code = result.returncode or int(static_failures != 0)
    receipt = dict(schema=1,kind='production_functions_with_modeled_MMIO_and_RCU',
                   hardware_test=False,exit_code=exit_code,
                   static_checks=[dict(name=label,passed=passed)
                                  for label, passed in static_checks],
                   sources={str(path.relative_to(root)):hashlib.sha256(path.read_bytes()).hexdigest()
                            for path in [base/'hnat_account.c',base/'hnat_nf_hook.c',base/'hnat.h',
                                         hnat_driver_path,
                                         npu_event_path,npu_core_path,npu_hook_path]},
                   output=result.stdout,stderr=result.stderr)
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True)
        a.output.write_text(json.dumps(receipt,indent=2)+'\n')
    raise SystemExit(exit_code)
