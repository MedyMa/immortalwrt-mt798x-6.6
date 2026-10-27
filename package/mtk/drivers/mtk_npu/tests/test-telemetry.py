#!/usr/bin/env python3
"""Test actual mailbox timeout and TRM arithmetic functions with HW modeled."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
src=Path(__file__).resolve().parents[1]/'src'
mailbox=(src/'mbox.c').read_text();trm=(src/'trm.c').read_text()

def function(source,name):
    match=re.search(r'(?m)^(?:static (?:inline )?)?(?:void|int|bool) '+name+r'\(',source)
    start=match.start();brace=source.index('{',match.end())
    masked=re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',lambda m:' '*len(m[0]),source,flags=re.S)
    depth=0
    for at in range(brace,len(source)):
        depth+=(masked[at]=='{')-(masked[at]=='}')
        if not depth:return source[start:at+1]+'\n'
    raise ValueError(name)

names=['mbox_send_msg_chk_timeout','__mbox_send_msg_no_wait_irq','mbox_send_msg','trm_cpu_utilization_ret_handler','mtk_trm_cpu_utilization']
functions=[function(mailbox,n) for n in names[:3]]+[function(trm,n) for n in names[3:]]
result_type=re.search(r'struct trm_cpu_utilization_result \{[^}]+\};',trm).group()
shim=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;typedef uint64_t u64;typedef long ktime_t;
enum core_id { CORE_OFFLOAD_0,CORE_OFFLOAD_1,CORE_OFFLOAD_2,CORE_OFFLOAD_3,CORE_MGMT };
struct mailbox_msg {u32 msg1,msg2,msg3,msg4;};
struct mailbox_dev {enum core_id core;unsigned cmd_id;};
struct mailbox_reg {u32 cmd_set_reg,msg_reg;};
typedef void (*mbox_ret_func_t)(void*,struct mailbox_msg*);
typedef struct {bool held;} spinlock_t;
static struct {struct {spinlock_t lock;} core[1][5];} mbox;
static struct {struct mailbox_dev mgmt_send_mdev,offload_send_mdev[4];} trm;
static unsigned ticks,writes,handlers,clears;static u32 command,idle,period;
static bool sent,ack,alive,omit_reply;static int reg_error;
#define MBOX_SEND 0
#define MBOX_SEND_TIMEOUT 2000
#define TRM_CMD_TYPE_CPU_UTILIZATION 1
#define BIT(i) (1U<<(i))
#define NPU_NOTICE(...) ((void)0)
#define NPU_ERR(...) ((void)0)
#define EXPORT_SYMBOL(...) /* no module loader in host model */
#define cpu_relax() ((void)0)
#define div_u64(n,d) ((n)/(d))
#define spin_lock_irqsave(l,f) do{assert(!(l)->held);(l)->held=true;(f)=0;}while(0)
#define spin_unlock_irqrestore(l,f) do{assert((l)->held&&(f)==0);(l)->held=false;}while(0)
static ktime_t ktime_get(void){ticks+=100;return ticks;}
static ktime_t ktime_sub(ktime_t a,ktime_t b){return a-b;}
static long ktime_to_us(ktime_t t){return t;}
static bool mtk_npu_mcu_alive(void){return alive;}
static int mbox_get_send_reg(struct mailbox_dev *d,struct mailbox_reg *r){
 if(reg_error)return reg_error;if(!d)return -EINVAL;r->cmd_set_reg=0;r->msg_reg=4;return 0;
}
static u32 mbox_read(u32 r){if(r==0){if(sent&&ack)command=0;return command;}return 0;}
static void mbox_write(u32 r,u32 value){writes++;if(r==0){command=value;sent=true;}}
static void mbox_post_send(u32 r,struct mailbox_msg *m,void *p,mbox_ret_func_t cb){
 (void)r;assert(command==0);clears++;
 if(cb&&!omit_reply){m->msg1=idle;m->msg2=period;handlers++;cb(p,m);}
}
'''
cases=r'''
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void reset(void){
 memset(&mbox,0,sizeof(mbox));memset(&trm,0,sizeof(trm));
 for(unsigned i=0;i<4;i++)trm.offload_send_mdev[i].core=i;
 trm.mgmt_send_mdev.core=CORE_MGMT;ticks=writes=handlers=clears=command=0;
 sent=false;ack=alive=true;omit_reply=false;reg_error=0;idle=20;period=100;
}
static void cb(void *p,struct mailbox_msg *m){(void)m;(*(unsigned*)p)++;}
int main(void){
 struct mailbox_dev dev={.core=CORE_OFFLOAD_0,.cmd_id=1};struct mailbox_msg msg={.msg1=1};
 unsigned calls=0;u32 value;
 reset();CHECK(mbox_send_msg(&dev,&msg,&calls,cb)==0);
 CHECK(calls==1&&handlers==1&&clears==1&&writes==6&&!mbox.core[0][0].lock.held);
 reset();command=4;ack=false;calls=0;
 CHECK(mbox_send_msg(&dev,&msg,&calls,cb)==-ETIMEDOUT);
 CHECK(!writes&&!calls&&!handlers&&!clears&&command==4&&!mbox.core[0][0].lock.held);
 CHECK(ticks<5000);
 reset();ack=false;calls=0;
 CHECK(mbox_send_msg(&dev,&msg,&calls,cb)==-ETIMEDOUT);
 CHECK(writes==6&&!calls&&!handlers&&!clears&&command==BIT(1)&&!mbox.core[0][0].lock.held);
 CHECK(ticks<5000);
 /* A new attempt must not overwrite the still-pending request. */
 sent=false;CHECK(mbox_send_msg(&dev,&msg,&calls,cb)==-ETIMEDOUT);
 CHECK(writes==6&&!calls&&!clears&&command==BIT(1));
 command=0;ack=true;CHECK(mbox_send_msg(&dev,&msg,&calls,cb)==0);
 CHECK(calls==1&&clears==1&&writes==12);
 reset();CHECK(mbox_send_msg(&dev,NULL,&calls,cb)==-EPERM);
 CHECK(!writes&&!clears&&!mbox.core[0][0].lock.held);
 reset();reg_error=-ENODEV;CHECK(mbox_send_msg(&dev,&msg,&calls,cb)==-ENODEV);
 CHECK(!writes&&!mbox.core[0][0].lock.held);
 for(unsigned c=0;c<=CORE_MGMT;c++){
  reset();CHECK(mtk_trm_cpu_utilization(c,&value)==0&&value==80);
  CHECK(handlers==1&&clears==1&&!mbox.core[0][c].lock.held);
 }
 const u32 samples[][3]={{0,1,100},{1,1,0},{0,UINT32_MAX,100},
 {UINT32_MAX,UINT32_MAX,0},{2147483647U,UINT32_MAX,50},{9,10,10},{1,3,66}};
 for(unsigned i=0;i<sizeof(samples)/sizeof(samples[0]);i++){
  reset();idle=samples[i][0];period=samples[i][1];
  CHECK(mtk_trm_cpu_utilization(CORE_MGMT,&value)==0&&value==samples[i][2]);
 }
 reset();period=0;CHECK(mtk_trm_cpu_utilization(0,&value)==-ERANGE);
 reset();idle=101;CHECK(mtk_trm_cpu_utilization(0,&value)==-ERANGE);
 reset();omit_reply=true;CHECK(mtk_trm_cpu_utilization(0,&value)==-ENODATA);
 reset();alive=false;CHECK(mtk_trm_cpu_utilization(0,&value)==-EAGAIN&&!writes);
 reset();CHECK(mtk_trm_cpu_utilization(-1,&value)==-EINVAL&&!writes);
 CHECK(mtk_trm_cpu_utilization(CORE_MGMT+1,&value)==-EINVAL&&!writes);
 CHECK(mtk_trm_cpu_utilization(0,NULL)==-EINVAL&&!writes);
 reset();ack=false;CHECK(mtk_trm_cpu_utilization(0,&value)==-ETIMEDOUT&&!handlers);
 printf("PASS %u assertions: actual mailbox timeout/response and TRM utilization arithmetic; MMIO, clocks and locks modeled\n",checks);
 return 0;
}
'''
c=a.output/'actual-telemetry.c';c.write_text(shim+result_type+'\n'+'\n'.join(functions)+cases)
commands=[]
for argv in [['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(c),'-o',str(a.output/'actual-telemetry')],[str(a.output/'actual-telemetry')]]:
    r=subprocess.run(argv,capture_output=True,text=True,timeout=10)
    commands.append({'argv':argv,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
    if r.returncode:break
receipt={'scope':__doc__,'commands':commands,'functions':[{'name':n,'sha256':hashlib.sha256(f.encode()).hexdigest()} for n,f in zip(names,functions)],'sources':{str(src/n):hashlib.sha256((src/n).read_bytes()).hexdigest()for n in ['mbox.c','trm.c']}}
(a.output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps({'commands':commands}));raise SystemExit(commands[-1]['exit'])
