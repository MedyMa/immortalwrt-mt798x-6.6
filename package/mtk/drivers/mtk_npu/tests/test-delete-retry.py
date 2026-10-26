#!/usr/bin/env python3
"""Run actual PCE/TOPS deletion functions with failed hardware services.

This checks reference ownership and retries, not firmware or packet forwarding.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--revision', help='Read production functions from this Git revision instead of the worktree')
args = p.parse_args()
root = Path(__file__).resolve().parent.parents[4]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)


def function(source, name):
    m = re.search(r'(?m)^(?:static )?(?:int|void|struct dipfilter_entry \*)\s*'
                  + re.escape(name) + r'\(', source)
    if not m:
        raise ValueError(name)
    opening = source.index('{', m.end())
    depth = 0
    for i in range(opening, len(source)):
        depth += (source[i] == '{') - (source[i] == '}')
        if depth == 0:
            return source[m.start():i + 1] + '\n'
    raise ValueError(name)


common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { \
 fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
typedef struct {bool held;} spinlock_t;
#define spin_lock_irqsave(l,f) do { (f)=0; CHECK(!(l)->held); (l)->held=true; } while (0)
#define spin_unlock_irqrestore(l,f) do { (void)(f); CHECK((l)->held); (l)->held=false; } while (0)
#define lockdep_assert_held(l) CHECK((l)->held)
#define PCE_NOTICE(...) ((void)0)
#define NPU_ERR(...) ((void)0)
#define NPU_NOTICE(...) ((void)0)
#define unlikely(x) (x)
#define FE_MEM_DIPFILTER_MAX_IDX 4
#define DIPFILTER_DISABLED 0
#define DIPFILTER_IPV4 1
#define DIPFILTER_IPV6 2
#define __DIPFILTER_TAG_MAX 3
#define FE_MEM_CMD_WRITE 1
#define FE_MEM_TYPE_DIPFILTER 1
typedef struct {unsigned value;} refcount_t;
static void refcount_set(refcount_t *r,unsigned v){r->value=v;}
static unsigned refcount_read(const refcount_t *r){return r->value;}
static void refcount_inc(refcount_t *r){CHECK(r->value>0);r->value++;}
static void refcount_dec(refcount_t *r){CHECK(r->value>1);r->value--;}
static bool refcount_dec_and_test(refcount_t *r){CHECK(r->value>0);return !--r->value;}
struct dip_desc {u32 tag,ipv4,ipv6[4];};
struct dipfilter_entry {struct dip_desc ddesc;refcount_t refcnt;u32 index;};
struct {struct dipfilter_entry dip_entries[4];unsigned long dip_tbl[1];spinlock_t lock;} dip_hw;
#define for_each_set_bit(i,p,n) for ((i)=0;(i)<(n);(i)++) if (*(p)&(1UL<<(i)))
static unsigned find_first_zero_bit(unsigned long *p,unsigned n){unsigned i;for(i=0;i<n;i++)if(!(*p&(1UL<<i)))break;return i;}
static void set_bit(unsigned i,unsigned long *p){*p|=1UL<<i;}
static void clear_bit(unsigned i,unsigned long *p){*p&=~(1UL<<i);}
struct fe_mem_msg {u32 index;union {struct dip_desc ddesc;u32 raw[6];};};
static int write_error;static unsigned hw_writes;
static void mtk_pce_fe_mem_msg_config(struct fe_mem_msg *m,int cmd,int type,u32 i){(void)cmd;(void)type;memset(m,0,sizeof(*m));m->index=i;}
static int mtk_pce_fe_mem_msg_send(struct fe_mem_msg *m){(void)m;++hw_writes;return write_error;}
'''
def source_text(path):
    if args.revision:
        return subprocess.check_output(['git', 'show', args.revision + ':' + path],
                                       cwd=root, text=True)
    return (root / path).read_text()


pce_source = source_text('package/mtk/drivers/pce/src/dipfilter.c')
npu_source = source_text('package/mtk/drivers/mtk_npu/src/tnl_offload.c')
pce_names = ['__mtk_pce_dip_info_find', '__mtk_pce_dipfilter_entry_add',
             'mtk_pce_dipfilter_entry_add', 'mtk_pce_dipfilter_entry_del']
npu_names = ['__mtk_npu_tnl_sync_param_delete', 'mtk_npu_tnl_sync_param_delete']
functions = [function(pce_source, n) for n in pce_names]
npu = [function(npu_source, n) for n in npu_names]
services = r'''
struct npu_tnl_params {u32 address;};
struct npu_tnl_info {bool pce_ready,freed;u32 tnl_idx;struct npu_tnl_params cache,tnl_params;};
struct net_cmd {int type,sub_type,arg[2];};
#define NPU_NET_CMD_TYPE_TNL 1
#define NPU_TNL_NET_CMD_PARAMS 1
#define NPU_TNL_PARAMS_NET_CMD_DELETE 1
#define NPU_TNL_PARAMS_NET_CMD_INVALIDATE 2
static int pause_error,mgmt_error,offload_error;
static unsigned pauses,resumes,cls_teardowns,invalidations;
static int mtk_npu_net_dev_pause(void){++pauses;return pause_error;}
static void mtk_npu_net_dev_resume(void){++resumes;}
static int mtk_npu_net_send_cmd_mgmt(struct net_cmd *c){(void)c;return mgmt_error;}
static int mtk_npu_net_send_cmd_offload_no_wait(struct net_cmd *c){(void)c;++invalidations;return offload_error;}
static void tnl_params_fw_write(struct npu_tnl_info *i){CHECK(!i->tnl_params.address);}
static int mtk_npu_tnl_info_dipfilter_tear_down(struct npu_tnl_info *i){struct dip_desc d={.tag=DIPFILTER_IPV4,.ipv4=i->tnl_params.address};return mtk_pce_dipfilter_entry_del(&d);}
static int mtk_npu_tnl_info_cls_tear_down(struct npu_tnl_info *i){(void)i;++cls_teardowns;return 0;}
static void mtk_npu_tnl_info_tcls_invalidate(struct npu_tnl_info *i){(void)i;++cls_teardowns;}
static void mtk_npu_tnl_info_free(struct npu_tnl_info *i){CHECK(!i->freed);i->freed=true;}
'''
cases = r'''
static void reset(void){memset(&dip_hw,0,sizeof(dip_hw));for(unsigned i=0;i<4;i++)dip_hw.dip_entries[i].index=i;
 write_error=pause_error=mgmt_error=offload_error=0;hw_writes=pauses=resumes=cls_teardowns=invalidations=0;}
static struct dip_desc desc={.tag=DIPFILTER_IPV4,.ipv4=0xc0000201};
static unsigned refs(void){return dip_hw.dip_entries[0].refcnt.value;}
static void pce_retry(void){
 reset();CHECK(!mtk_pce_dipfilter_entry_add(&desc));CHECK(refs()==1);
 for(unsigned i=0;i<3;i++){write_error=-ETIMEDOUT;CHECK(mtk_pce_dipfilter_entry_del(&desc)==-ETIMEDOUT);CHECK(refs()==1);CHECK(dip_hw.dip_tbl[0]==1);}
 write_error=0;CHECK(!mtk_pce_dipfilter_entry_del(&desc));CHECK(refs()==0);CHECK(!dip_hw.dip_tbl[0]);
 CHECK(!mtk_pce_dipfilter_entry_add(&desc));CHECK(refs()==1);CHECK(!mtk_pce_dipfilter_entry_del(&desc));
 reset();write_error=-EIO;CHECK(mtk_pce_dipfilter_entry_add(&desc)==-EIO);CHECK(!dip_hw.dip_tbl[0]);
 reset();CHECK(!mtk_pce_dipfilter_entry_add(&desc));CHECK(!mtk_pce_dipfilter_entry_add(&desc));
 unsigned writes=hw_writes;CHECK(!mtk_pce_dipfilter_entry_del(&desc));CHECK(refs()==1);CHECK(hw_writes==writes);
 CHECK(!mtk_pce_dipfilter_entry_del(&desc));CHECK(!dip_hw.dip_tbl[0]);
}
static void npu_retry(unsigned phase,unsigned shared){
 reset();for(unsigned i=0;i<shared;i++)CHECK(!mtk_pce_dipfilter_entry_add(&desc));
 struct npu_tnl_info info={.pce_ready=true,.cache={desc.ipv4},.tnl_params={desc.ipv4}};
 if(phase==0)pause_error=-ETIMEDOUT;
 if(phase==1)mgmt_error=-ETIMEDOUT;
 if(phase==2)offload_error=-ETIMEDOUT;
 for(unsigned retry=0;retry<3;retry++) {
  info.tnl_params=info.cache; /* worker reloads cache before every attempt */
  CHECK(mtk_npu_tnl_sync_param_delete(false,&info)==-ETIMEDOUT);
  CHECK(!info.freed);CHECK(refs()==shared-1);
 }
 if(phase==0)pause_error=0;
 if(phase==1)mgmt_error=0;
 if(phase==2)offload_error=0;
 info.tnl_params=info.cache; /* actual worker reloads cache before every retry */
 CHECK(!mtk_npu_tnl_sync_param_delete(false,&info));CHECK(info.freed);CHECK(refs()==shared-1);CHECK(cls_teardowns==1);
 if(shared==2) {
  struct npu_tnl_info other={.pce_ready=true,.cache={desc.ipv4},.tnl_params={desc.ipv4}};
  CHECK(!mtk_npu_tnl_sync_param_delete(false,&other));CHECK(other.freed);
  CHECK(!dip_hw.dip_tbl[0]);CHECK(refs()==0);CHECK(cls_teardowns==2);
 }
 CHECK(!dip_hw.lock.held);
}
static void dip_failure(void){
 reset();CHECK(!mtk_pce_dipfilter_entry_add(&desc));
 struct npu_tnl_info info={.pce_ready=true,.cache={desc.ipv4},.tnl_params={desc.ipv4}};
 write_error=-EIO;CHECK(mtk_npu_tnl_sync_param_delete(false,&info)==-EIO);
 CHECK(info.pce_ready&&!info.freed);CHECK(refs()==1);CHECK(!pauses);
 write_error=0;CHECK(!mtk_npu_tnl_sync_param_delete(false,&info));CHECK(info.freed);CHECK(!dip_hw.dip_tbl[0]);
}
int main(int argc,char **argv){
 if(argc>1&&!strcmp(argv[1],"pce"))pce_retry();
 else if(argc>1&&!strcmp(argv[1],"npu"))npu_retry(1,2);
 else {pce_retry();for(unsigned p=0;p<3;p++)for(unsigned n=1;n<3;n++)npu_retry(p,n);dip_failure();}
 printf("PASS %u assertions: actual deletion functions; locks/MMIO/mailbox/device services modeled; no packet offload claim\n",checks);
 return 0;
}
'''
code = common + '\n'.join(functions) + services + '\n'.join(npu) + cases
file = out / 'actual-delete-retry.c'
file.write_text(code)
commands = []
compile_cmd = ['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-fsanitize=address,undefined',
               '-fno-omit-frame-pointer', str(file), '-o', str(out / 'actual-delete-retry')]
for cmd in [compile_cmd, [str(out / 'actual-delete-retry'), 'pce'],
            [str(out / 'actual-delete-retry'), 'npu'], [str(out / 'actual-delete-retry')]]:
    r = subprocess.run(cmd, capture_output=True, text=True)
    commands.append(dict(argv=cmd, exit=r.returncode, stdout=r.stdout, stderr=r.stderr))
    if len(commands) == 1 and r.returncode:
        break
receipt = dict(scope=__doc__, revision=args.revision or 'worktree', functions=[dict(name=n, sha256=hashlib.sha256(f.encode()).hexdigest())
              for n, f in zip(pce_names+npu_names, functions+npu)], commands=commands)
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(commands, indent=2))
raise SystemExit(0 if len(commands) == 4 and all(c['exit'] == 0 for c in commands) else 1)
