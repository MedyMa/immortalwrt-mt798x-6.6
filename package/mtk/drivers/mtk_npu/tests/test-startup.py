#!/usr/bin/env python3
"""Execute real probe/post-init/remove control flow with dependency failures."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);args=p.parse_args();V=Path(__file__).resolve().parent;O=args.output.resolve();O.mkdir(parents=True,exist_ok=True);source=(V.parent/'src/init.c').read_text()
def function(name):
 m=re.search(r'(?m)^static (?:int|void) '+name+r'\(',source);assert m,name
 start=m.start();opening=source.index('{',m.end());depth=0
 for i in range(opening,len(source)):
  if source[i]=='{':depth+=1
  elif source[i]=='}':
   depth-=1
   if depth==0:return source[start:i+1]+'\n'
names=['mtk_npu_post_init','mtk_npu_post_deinit','mtk_npu_probe'];funcs=[function(n) for n in names]
# Keep the actual version conditional around the 6.6/6.11 remove signatures.
remove=source[source.index('#if KERNEL_VERSION(6, 11, 0) > LINUX_VERSION_CODE'):source.index('static struct npu_init_data mt7988_npu_init_data')]
prefix=r'''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define CONFIG_MTK_NPU_HNAT 1
#define IS_ENABLED(x) (x)
#define KERNEL_VERSION(a,b,c) (((a)<<16)|((b)<<8)|(c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6,6,94)
#define IORESOURCE_MEM 0
#define NPU_ERR(...) ((void)0)
#define NPU_INFO(...) ((void)0)
#define smp_store_release(p,v) (*(p)=(v))
struct device{int unused;};struct platform_device{struct device dev;void *drvdata;};
struct resource{unsigned long start,end;};
struct npu_init_data{int(*plat_net_dev_init)(struct platform_device *);};
struct {struct device *dev;void *base;unsigned long phys_base,size;} npu;
static bool npu_ready;static int fail_step,step,mode,failure_errno=-EIO;static unsigned checks;
static const char *events[160];static unsigned event_count;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static int record_init(const char *name){events[event_count++]=name;return ++step==fail_step?failure_errno:0;}
static void record_end(const char *name){events[event_count++]=name;}
static int dev_err_probe(struct device *d,int ret,const char *s){(void)d;(void)s;return ret;}
static int mtk_npu_link_supplier(struct platform_device *d,const char *n){(void)d;(void)n;return record_init("supplier");}
static struct resource resource={.start=1,.end=1024};
static struct resource *platform_get_resource_byname(struct platform_device *p,int type,const char *name){(void)p;(void)type;(void)name;return mode==1?NULL:&resource;}
static unsigned long resource_size(struct resource *r){return r->end-r->start+1;}
static void *devm_ioremap(struct device *d,unsigned long start,unsigned long size){(void)d;(void)start;(void)size;return mode==2?NULL:&resource;}
static void platform_set_drvdata(struct platform_device *p,void *d){p->drvdata=d;}
'''
initializers=['mbox_init','fw_init','trm_init','mcu_init','netsys_init','net_core_init','tdma_init','tnl_offload_init','misc_init','logger_init','mcu_bring_up','tnl_offload_post_init','netevent_register','sysfs_init','sysfs_net_init','ser_init','wdt_init','thermal_init','procfs_init','debugfs_init','statistic_init','nf_hnat_start']
finalizers=['trm_deinit','mcu_deinit','netsys_deinit','net_core_deinit','tnl_offload_deinit','misc_deinit','logger_deinit','mcu_tear_down','netevent_unregister','sysfs_deinit','sysfs_net_deinit','ser_deinit','wdt_deinit','thermal_deinit','procfs_deinit','debugfs_deinit','statistic_deinit']
for n in initializers:prefix+=f'static int mtk_npu_{n}(struct platform_device *d){{(void)d;return record_init("{n}");}}\n'
for n in finalizers:prefix+=f'static void mtk_npu_{n}(struct platform_device *d){{(void)d;record_end("{n}");}}\n'
prefix+='static int mtk_npu_trm_fs_init(void){return record_init("trm_fs_init");}\nstatic void mtk_npu_trm_fs_deinit(void){record_end("trm_fs_deinit");}\n'
prefix+='static void mtk_npu_tnl_offload_stop_worker(void){record_end("stop_worker");}\nstatic void mtk_npu_nf_hnat_quiesce(void){CHECK(!npu_ready);record_end("quiesce");}\nstatic void mtk_npu_nf_hnat_finish(void){record_end("finish");}\n'
prefix+='static struct npu_init_data init_data={.plat_net_dev_init=mtk_npu_tdma_init};\nstatic const void *of_device_get_match_data(struct device *d){(void)d;return &init_data;}\n'
tests=r'''
static int position(const char *s){for(unsigned i=0;i<event_count;i++)if(!strcmp(events[i],s))return i;return -1;}
static void reset(void){event_count=0;step=0;mode=0;fail_step=0;failure_errno=-EIO;npu_ready=false;}
int main(void){struct platform_device p={0};reset();CHECK(!mtk_npu_probe(&p));int init_steps=step;
 CHECK(npu_ready&&p.drvdata==&npu);CHECK(position("nf_hnat_start")>=0);
 event_count=0;CHECK(!mtk_npu_remove(&p));CHECK(!npu_ready&&!p.drvdata);
 CHECK(position("quiesce")<position("stop_worker"));CHECK(position("stop_worker")<position("mcu_tear_down"));
 CHECK(position("procfs_deinit")<position("tnl_offload_deinit"));CHECK(position("tnl_offload_deinit")<position("finish"));
 for(int i=1;i<=init_steps;i++){reset();fail_step=i;p.drvdata=NULL;CHECK(mtk_npu_probe(&p)==-EIO);CHECK(!npu_ready&&!p.drvdata);
  CHECK(position("nf_hnat_start")<0||i==init_steps);if(position("mcu_bring_up")>=0&&i>14)CHECK(position("logger_deinit")>=0);}
 reset();fail_step=1;failure_errno=-517;CHECK(mtk_npu_probe(&p)==-517);CHECK(!npu_ready&&!p.drvdata);
 reset();fail_step=14;failure_errno=-ENOENT;CHECK(mtk_npu_probe(&p)==-ENOENT);CHECK(!npu_ready&&!p.drvdata);CHECK(position("nf_hnat_start")<0);
 reset();mode=1;CHECK(mtk_npu_probe(&p)==-ENXIO);CHECK(!npu_ready&&!p.drvdata);
 reset();mode=2;CHECK(mtk_npu_probe(&p)==-ENOMEM);CHECK(!npu_ready&&!p.drvdata);
 printf("PASS %u assertions: actual default-startup probe/post-init/remove; %d injected dependency failures, two resource failures; services shimmed\n",checks,init_steps);return 0;}
'''
generated=prefix+'\n'.join(funcs)+remove+tests;path=O/'actual-startup.c';path.write_text(generated);rows=[]
for cmd in [['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(path),'-o',str(O/'actual-startup')],[str(O/'actual-startup')]]:
 r=subprocess.run(cmd,capture_output=True,text=True);rows.append({'argv':cmd,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
 if r.returncode:break
rec={'scope':'real startup state/control flow with injected API failures; component internals, power/MMIO, firmware and device links shimmed. Not a board boot/resource-leak proof.','source_sha256':hashlib.sha256(source.encode()).hexdigest(),'functions':[{'name':n,'sha256':hashlib.sha256(f.encode()).hexdigest()} for n,f in zip(names+['mtk_npu_remove'],funcs+[remove])],'test_sha256':hashlib.sha256(generated.encode()).hexdigest(),'commands':rows};(O/'receipt.json').write_text(json.dumps(rec,indent=2)+'\n');print(json.dumps({'commands':rows}));assert len(rows)==2 and all(x['exit']==0 for x in rows)
