#!/usr/bin/env python3
"""Compile and execute actual HNAT gates, CT reset helpers and MacTable one-shot logic."""
import argparse, json, re, subprocess, tempfile
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--kernel-tree',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[2]
hook=(root/'target/linux/mediatek/files-6.6/drivers/net/ethernet/mediatek/mtk_hnat/hnat_nf_hook.c').read_text()
header=(a.kernel_tree/'include/net/netfilter/nf_conntrack_acct.h').read_text()
def function(source,name):
    m=re.search(r'(?m)^(?:static (?:inline )?)?(?:bool|void|u64) '+name+r'\(',source);assert m,name
    start=source.index('{',m.start());depth=1;end=start+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[m.start():end]
def structure(source,name):
    m=re.search(r'(?m)^struct '+name+r' \{',source);assert m,name
    end=source.index('\n};',m.start())+3
    return source[m.start():end]
program=r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef uint32_t u32; typedef uint64_t u64; typedef uint8_t u8;
#define CONFIG_MEDIATEK_NETSYS_V3 1
#define CONFIG_NF_HNAT_PACKET_ACCOUNTING 1
#define BIT(n) (1U<<(n))
#define GENMASK(h,l) ((uint32_t)((~0ULL<<(l))&(~0ULL>>(63-(h)))))
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
enum {INVALID=0,UNBIND=1,BIND=2,IPV4_HNAPT=0,IPV4_HNAT=1,IPV4_DSLITE=3,IPV4_MAP_T=8,IPV4_MAP_E=9,IPV6_3T_ROUTE=4,IPV6_5T_ROUTE=5,IPV6_6RD=7,IPV6_HNAPT=18,IPV6_HNAT=19};
struct info { u32 timestamp:8,sp:4,count:8,ilgf:1,mc:1,preb:1,pkt_type:5,state:2,udp:1,sta:1; };
struct hnat_ipv4_hnapt { union {struct info udib1,bfib1;u32 info_blk1;};u32 sip,dip;uint16_t dport,sport;u32 info_blk2; };
struct hnat_ipv6_3t_route {union {struct info udib1,bfib1;u32 info_blk1;};u32 ipv6_sip0,words[7],resv1;};
struct hnat_ipv6_5t_route {union {struct info udib1,bfib1;u32 info_blk1;};u32 ipv6_sip0,words[8],resv1;};
struct foe_entry {union {struct info udib1,bfib1;struct hnat_ipv4_hnapt ipv4_hnapt;struct hnat_ipv6_3t_route ipv6_3t_route;struct hnat_ipv6_5t_route ipv6_5t_route;};};
static struct foe_entry *mutate;
#define dma_rmb() do {if(mutate){mutate->bfib1.state=BIND;mutate=NULL;}}while(0)
'''
program+='\n'.join(function(hook,n)for n in ['hnat_flow_entry_format_compatible','hnat_flow_entry_match','hnat_flow_entry_unbind_empty','hnat_flow_entry_unbind_match'])
program+=r'''
int main(void){
 struct foe_entry live={},candidate={};live.bfib1.state=UNBIND;candidate.bfib1.pkt_type=IPV4_HNAPT;
 candidate.ipv4_hnapt.sip=1;candidate.ipv4_hnapt.dip=2;candidate.ipv4_hnapt.sport=1234;candidate.ipv4_hnapt.dport=443;
 assert(hnat_flow_entry_unbind_match(&candidate,&live));
 live.ipv4_hnapt=candidate.ipv4_hnapt;live.bfib1.state=UNBIND;
 assert(hnat_flow_entry_unbind_match(&candidate,&live));
 live.ipv4_hnapt.dip=3;assert(!hnat_flow_entry_unbind_match(&candidate,&live));
 live.ipv4_hnapt.dip=2;live.udib1.udp=1;assert(!hnat_flow_entry_unbind_match(&candidate,&live));
 memset(&live,0,sizeof(live));assert(!hnat_flow_entry_unbind_match(&candidate,&live));
 live.bfib1.state=BIND;assert(!hnat_flow_entry_unbind_match(&candidate,&live));
 live.bfib1.state=UNBIND;live.udib1.sta=1;assert(!hnat_flow_entry_unbind_match(&candidate,&live));
 live.udib1.sta=0;candidate.bfib1.pkt_type=IPV6_5T_ROUTE;assert(!hnat_flow_entry_unbind_match(&candidate,&live));
 candidate.bfib1.pkt_type=IPV4_HNAPT;mutate=&live;assert(!hnat_flow_entry_unbind_match(&candidate,&live));
 return 0;
}
'''
counter=r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
typedef uint64_t u64;typedef uint32_t u32;typedef unsigned int seqlock_t;
typedef struct {uint64_t value;} atomic64_t;
#define IP_CT_DIR_MAX 2
#define CONFIG_NF_HNAT_PACKET_ACCOUNTING 1
static u64 atomic64_xchg(atomic64_t *a,u64 n){u64 old=a->value;a->value=n;return old;}
static u64 atomic64_read(const atomic64_t *a){return a->value;}
#define write_seqlock_bh(p) ((void)(p))
#define write_sequnlock_bh(p) ((void)(p))
#define read_seqbegin(p) (0U)
#define read_seqretry(p,s) ((void)(p),(void)(s),false)
'''
counter+='\n'.join(structure(header,n)for n in ['nf_conn_counter','nf_ct_acct_totals','nf_conn_acct'])
counter+='\n'+function(header,'nf_ct_acct_read_reset')+'\n'+function(header,'nf_ct_acct_read_totals')
counter+=r'''
int main(void){struct nf_ct_acct_totals total={};struct nf_conn_acct acct={.total=&total};u64 p,b,tp,tb;
 for(unsigned int i=0;i<20;i++){acct.counter[0].packets.value+=123;acct.counter[0].bytes.value+=98765;
 nf_ct_acct_read_reset(&acct,0,&p,&b);assert(p==123&&b==98765);
 assert(nf_ct_acct_read_totals(&acct,0,&tp,&tb));assert(tp==123ULL*(i+1)&&tb==98765ULL*(i+1));}
 acct.counter[1].packets.value=(1ULL<<55)+3;acct.counter[1].bytes.value=(1ULL<<60)+7;
 nf_ct_acct_read_reset(&acct,1,&p,&b);acct.counter[1].packets.value=5;acct.counter[1].bytes.value=9;
 assert(nf_ct_acct_read_totals(&acct,1,&tp,&tb));assert(tp==(1ULL<<55)+8&&tb==(1ULL<<60)+16);
 acct.total=0;assert(!nf_ct_acct_read_totals(&acct,0,&tp,&tb));nf_ct_acct_read_reset(&acct,0,&p,&b);
 return 0;}
'''
one_prefix=r'''
#include <pthread.h>
#include <string.h>
#include <assert.h>
#define BOOLEAN int
#define TRUE 1
static int test_and_set_bit(int b,unsigned long *p){return !!(__atomic_fetch_or(p,1UL<<b,__ATOMIC_SEQ_CST)&(1UL<<b));}
static int gate(char *arg){
'''
one_suffix=r'''
 return 0;}
static unsigned int prints;
static void *worker(void *unused){(void)unused;for(int i=0;i<1000;i++)if(!gate(NULL))__atomic_fetch_add(&prints,1,__ATOMIC_SEQ_CST);return NULL;}
int main(void){pthread_t threads[16];for(int i=0;i<16;i++)assert(!pthread_create(&threads[i],0,worker,0));for(int i=0;i<16;i++)pthread_join(threads[i],0);assert(prints==1);for(int i=0;i<100;i++)assert(!gate("ap"));return 0;}
'''
with tempfile.TemporaryDirectory(prefix='mtk-host-tests-')as tmp:
    programs=[('flow-identity',program),('reset-carry',counter)]
    for driver in ['mt_hwifi','mt_wifi7']:
        mac=(root/('package/mtk/drivers/'+driver+'/src/mt_wifi/common/cmm_info.c')).read_text()
        start=mac.index('static unsigned long mac_table_dumped;');end=mac.index('\n\tMTWF_PRINT',start)
        programs.append(('log-once-'+driver,one_prefix+mac[start:end]+one_suffix))
    for name,code in programs:
        src=Path(tmp)/(name+'.c');binary=Path(tmp)/name;src.write_text(code)
        subprocess.run(['cc','-std=gnu11','-Wall','-Werror','-Wno-unused-variable','-pthread',str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
assert 'trace_printk(' not in hook
print(json.dumps({'programs':len(programs),'failures':0,'scope':'actual production helper logic and concurrent guards in both WiFi source copies; no MMIO or hardware behavior simulated as verified'}))
