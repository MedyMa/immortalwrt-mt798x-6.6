#!/usr/bin/env python3
"""Exercise real synchronization control flow; firmware/PCE services modeled."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[5]
src = root/'package/mtk/drivers/mtk_npu/src'
text = (src/'tnl_offload.c').read_text()
header = (src/'inc/npu/tunnel.h').read_text()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)

def block(source, name, enum=False):
    pattern = r'(?m)^enum ' + name + r'\s*\{' if enum else r'(?m)^static (?:int|void) ' + name + r'\('
    start = re.search(pattern, source).start()
    brace = source.index('{', start)
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"', lambda m: ' '*len(m[0]), source, flags=re.S)
    depth = 0
    for at in range(brace, len(source)):
        depth += (masked[at] == '{') - (masked[at] == '}')
        if not depth:
            return source[start:at+1] + (';' if enum else '') + '\n'
    raise ValueError(name)

names = ['tnl_sync_stage_set', 'tnl_sync_attempt_begin', 'tnl_sync_attempt_finish',
         '__mtk_npu_tnl_sync_param_update', 'mtk_npu_tnl_sync_param_update']
functions = [block(text, n) for n in names]
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
typedef struct { bool held; } spinlock_t;
#define spin_lock_irqsave(l,f) do{assert(!(l)->held);(l)->held=true;(f)=0;}while(0)
#define spin_unlock_irqrestore(l,f) do{assert((l)->held&&(f)==0);(l)->held=false;}while(0)
#define NPU_ERR(...) ((void)0)
#define NPU_NOTICE(...) ((void)0)
#define NPU_TNL_NET_CMD_PARAMS 0
#define NPU_NET_CMD_TYPE_TNL 0
#define NPU_TNL_PARAMS_NET_CMD_UPDATE 0
#define NPU_TNL_PARAMS_NET_CMD_INVALIDATE 1
struct npu_tnl_info;
struct npu_tnl_info_ops { void (*flush)(struct npu_tnl_info *); };
struct npu_tnl_info {
 spinlock_t lock;u64 generation,sync_target_generation,synced_generation;
 u64 sync_attempts,sync_failures;int last_sync_error;u8 sync_stage;
 u32 tnl_idx;bool pce_ready,ready;struct npu_tnl_info_ops *ops;
};
struct net_cmd {unsigned type,sub_type,arg[4];};
static struct npu_tnl_info *current;
static int failure_stage;
static unsigned resumes,teardowns,flushes;
static bool advance_generation;
static int mtk_npu_tnl_info_cls_setup(struct npu_tnl_info *i) {(void)i;return failure_stage==1?-ENOMEM:0;}
static int mtk_npu_net_dev_pause(void) {return failure_stage==2?-EBUSY:0;}
static void tnl_params_fw_write(struct npu_tnl_info *i) {(void)i;}
static int mtk_npu_net_send_cmd_mgmt(struct net_cmd *c) {
 (void)c;if(advance_generation)current->generation++;
 return failure_stage==3?-ETIMEDOUT:0;
}
static int mtk_npu_net_send_cmd_offload_no_wait(struct net_cmd *c) {(void)c;return failure_stage==4?-EIO:0;}
static void mtk_npu_net_dev_resume(void) {resumes++;}
static int mtk_npu_tnl_info_dipfilter_setup(struct npu_tnl_info *i) {(void)i;return failure_stage==5?-ENOSPC:0;}
static void mtk_npu_tnl_info_cls_tear_down(struct npu_tnl_info *i) {(void)i;teardowns++;}
static void tnl_info_sta_updated(struct npu_tnl_info *i) {assert(!i->lock.held);i->ready=true;}
static void flush(struct npu_tnl_info *i) {assert(!i->lock.held);flushes++;}
'''
cases = r'''
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void) {
 struct npu_tnl_info info;struct npu_tnl_info_ops ops={.flush=flush};
 for(int stage=1;stage<=5;stage++) {
  memset(&info,0,sizeof(info));current=&info;info.generation=7;info.sync_target_generation=7;
  failure_stage=stage;resumes=teardowns=flushes=0;
  CHECK(mtk_npu_tnl_sync_param_update(&info,true,true)<0);
  CHECK(info.sync_stage==stage&&info.last_sync_error<0);
  CHECK(info.sync_attempts==1&&info.sync_failures==1);
  CHECK(!info.ready&&!info.pce_ready&&info.synced_generation==0&&!info.lock.held);
  CHECK(resumes==(stage>=3));
  CHECK(teardowns==(stage>=2&&stage<=4));
  failure_stage=0;CHECK(mtk_npu_tnl_sync_param_update(&info,true,true)==0);
  CHECK(info.sync_stage==NPU_TNL_SYNC_READY&&info.last_sync_error==0);
  CHECK(info.sync_attempts==2&&info.sync_failures==1);
  CHECK(info.ready&&info.pce_ready&&info.synced_generation==7&&!info.lock.held);
 }
 memset(&info,0,sizeof(info));current=&info;info.generation=7;info.sync_target_generation=7;
 info.ops=&ops;advance_generation=true;flushes=0;
 CHECK(mtk_npu_tnl_sync_param_update(&info,false,false)==0);
 CHECK(info.generation==8&&info.synced_generation==7);
 CHECK(flushes==1&&info.sync_attempts==1&&info.sync_failures==0);
 CHECK(!info.pce_ready&&info.ready);
 printf("PASS %u assertions: actual tunnel sync stage/errno/attempt journal and control flow; PCE/mailbox/device services modeled\n",checks);
 return 0;
}
'''
c = out/'actual-sync-health.c'
c.write_text(shim + block(header, 'npu_tnl_sync_stage', True) + '\n'.join(functions) + cases)
commands = []
for argv in [['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
              '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(c), '-o', str(out/'actual-sync-health')],
             [str(out/'actual-sync-health')]]:
    r = subprocess.run(argv, capture_output=True, text=True)
    commands.append({'argv': argv, 'exit': r.returncode, 'stdout': r.stdout, 'stderr': r.stderr})
    if r.returncode:
        break
receipt = {'scope': __doc__, 'commands': commands, 'source_sha256': hashlib.sha256(text.encode()).hexdigest(),
           'functions': [{'name': n, 'sha256': hashlib.sha256(f.encode()).hexdigest()} for n,f in zip(names,functions)],
           'hardware_forwarding_proof': False}
(out/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
print(json.dumps(receipt))
raise SystemExit(commands[-1]['exit'])
