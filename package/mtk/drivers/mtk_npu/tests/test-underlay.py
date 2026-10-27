#!/usr/bin/env python3
"""Verbatim HNAT egress and native VLAN/NPU lease regressions, with services shimmed."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess,tempfile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--kernel',type=Path,required=True);p.add_argument('--baseline-vlan',type=Path);p.add_argument('--output',type=Path,required=True);args=p.parse_args()
V=Path(__file__).resolve().parent;R=V.parents[4];H=R/'target/linux/mediatek/files-6.6/drivers/net/ethernet/mediatek/mtk_hnat';N=V.parent/'src';O=args.output.resolve();O.mkdir(parents=True,exist_ok=True)
def block(s,name,kind='function'):
 if kind=='struct':m=re.search(r'(?m)^struct '+re.escape(name)+r'\s*\{',s)
 elif kind=='enum':m=re.search(r'(?m)^enum '+re.escape(name)+r'\s*\{',s)
 else:m=re.search(r'(?m)^(?:static\s+)?(?:inline\s+)?(?:(?:bool|void|int)\s+|struct npu_tnl_info\s*\*\s*)'+re.escape(name)+r'\s*\(',s)
 if not m:raise ValueError(name)
 start=m.start();brace=s.index('{',m.end()-1);mask=re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',lambda m:' '*len(m[0]),s,flags=re.S);depth=0
 for i in range(brace,len(s)):
  if mask[i]=='{':depth+=1
  elif mask[i]=='}':
   depth-=1
   if depth==0:
    if kind in ['struct','enum']:i=s.index(';',i)
    return s[start:i+1]+'\n'
 raise ValueError(name)
header=(H/'hnat.h').read_text();hook=(H/'hnat_nf_hook.c').read_text();tnl=(N/'inc/npu/tunnel.h').read_text();ac=(N/'npu-nf_hnat/tnl-offload.c').read_text();core=(N/'tnl_offload.c').read_text()
types=block((args.kernel/'include/linux/netdevice.h').read_text(),'net_device_path_type','enum')+block((args.kernel/'include/net/netfilter/nf_flow_table.h').read_text(),'flow_offload_hw_path','struct')
start=header.index('#if defined(CONFIG_MEDIATEK_NETSYS_V2) || defined(CONFIG_MEDIATEK_NETSYS_V3)',header.index('/* Descriptor Structure */'));types+=header[start:header.index('struct hnat_flow_entry {',start)]+block(header,'hnat_tunnel_token','struct')
for f,n in [('network/ip_params.h','npu_ip_params'),('network/ipv6_params.h','npu_ipv6_params'),('transport/udp_params.h','npu_udp_params'),('tunnel/l2tp/l2tp_params.h','npu_l2tp_params'),('tunnel/pptp/pptp_params.h','npu_pptp_params'),('tunnel/vxlan/vxlan_params.h','npu_vxlan_params')]:types+=block((N/'protocol/inc/npu/protocol'/f).read_text(),n,'struct')
params=(N/'inc/npu/npu_params.h').read_text();types+=params[params.index('enum npu_mac_type {'):params.index('int\nmtk_npu_encap_param_setup')]
types+='\n'.join(l for l in params.splitlines() if l.startswith(('#define TNL_DECAP_ENABLE','#define TNL_ENCAP_ENABLE')))+'\n'
types+=block(tnl,'tnl_status','enum')+block(tnl,'npu_tnl_sync_stage','enum')+block(tnl,'npu_tnl_info','struct')+block(tnl,'npu_tnl_input','struct')
typedef_start=tnl.index('typedef int (*tnl_info_setup_func_t)');types+=tnl[typedef_start:tnl.index(';',typedef_start)+1]+'\n'
types+='\n'.join(l for l in tnl.splitlines() if l.startswith('#define TNL_STA_'))+'\n'
dependency=r'''
struct npu_tnl_type {u16 max_mtu;void(*tnl_param_restore)(struct npu_params *,struct npu_params *);};
struct npu_tnl_info_ops {int unused;};static struct npu_tnl_info_ops hnat_tnl_info_ops;
static struct npu_tnl_info *test_info;static unsigned flushes,submits;static bool live_binding,ready=true,alive=true;
#define CONFIG_NPU_TNL_NUM 32
static struct {spinlock_t tbl_lock;struct npu_tnl_info *ht[4];u32 tbl_addr;struct npu_tnl_info tnl_infos[32];unsigned long tnl_used[1];} npu_tnl;
static struct {u64 phys_base,size;} npu;
struct platform_device {int unused;};
struct net_cmd {int type,sub_type;u32 arg[4],ret[4],return_cnt;};
#define NPU_NET_CMD_TYPE_TNL 1
#define NPU_TNL_NET_CMD_PARAMS 1
#define NPU_TNL_PARAMS_NET_CMD_BASE_ADDR_GET 0
#define NPU_NOTICE(...) ((void)0)
#define NPU_DBG(...) ((void)0)
#define NPU_ERR(...) ((void)0)
#define IS_ALIGNED(x,a) (!((x)&((a)-1)))
#define CONFIG_NPU_TNL_NUM 32
static int mcu_error;static u32 mcu_address,mcu_count=1;
static int mtk_npu_net_send_cmd_mgmt(struct net_cmd *c){c->ret[0]=mcu_address;c->return_cnt=mcu_count;return mcu_error;}
#define hash_for_each(ht,bkt,obj,member) for((bkt)=0;(bkt)<4;(bkt)++) if(((obj)=(ht)[bkt])!=NULL)
static bool mtk_npu_is_ready(void){return ready;}static bool mtk_npu_mcu_alive(void){return alive;}
static struct npu_tnl_info *mtk_npu_tnl_info_get_by_idx(u32 index){return index<32?test_info:(void *)-EINVAL;}
#define skb_hnat_is_encap(s) ((s)->encap)
#define skb_hnat_is_decap(s) ((s)->decap)
#define skb_set_npu_tnl_idx(s,i) ((s)->tops=(i)+32)
static void mtk_npu_tnl_info_hash_no_lock(struct npu_tnl_info *i){assert(i->lock.held);i->hlist.pprev=i;}
static void mtk_npu_tnl_info_submit_no_tnl_lock(struct npu_tnl_info *i){assert(i->lock.held);i->status|=TNL_STA_QUEUED;++submits;}
/* Actual HNAT flush nests entry/accounting _bh locks. Model their kernel
 * IRQ-enabled contract rather than aliasing _bh to a plain spin lock.
 */
static void mtk_npu_tnl_info_flush_one_ppe_tnl_no_lock(struct npu_tnl_info *i){assert(i->lock.held);spinlock_t ledger={0};spin_lock_bh(&ledger);spin_unlock_bh(&ledger);live_binding=false;++flushes;}
static int find_error,alloc_error;
static bool use_native_allocation;
static struct npu_tnl_info *real_alloc_no_lock(struct npu_tnl_type *t);
static struct npu_tnl_info *mtk_npu_tnl_info_find_no_lock(struct npu_tnl_type *t,struct npu_tnl_params *p){(void)t;(void)p;assert(npu_tnl.tbl_lock.held);return find_error?ERR_PTR(find_error):test_info;}
static struct npu_tnl_info *mtk_npu_tnl_info_alloc_no_lock(struct npu_tnl_type *t){assert(npu_tnl.tbl_lock.held);return use_native_allocation?real_alloc_no_lock(t):(alloc_error?ERR_PTR(alloc_error):test_info);}
#define lockdep_assert_held(s) assert((s)->held)
static unsigned long find_first_zero_bit(const unsigned long *bits,unsigned n){for(unsigned i=0;i<n;i++)if(!(*bits&(1UL<<i)))return i;return n;}
static void set_bit(unsigned i,unsigned long *bits){assert(i<32);*bits|=1UL<<i;}
static void clear_bit(unsigned i,unsigned long *bits){assert(i<32);*bits&=~(1UL<<i);}
#define INIT_HLIST_NODE(n) do{(n)->next=NULL;(n)->pprev=NULL;}while(0)
static bool hlist_unhashed(const struct hlist_node *n){return !n->pprev;}
static void hash_del(struct hlist_node *n){INIT_HLIST_NODE(n);}
'''
funcs=[];meta=[]
for text,names in [(hook,['hnat_tunnel_outer_neighbour','hnat_tunnel_outer_path','hnat_tunnel_outer_l2']),((args.kernel/'net/8021q/vlan_dev.c').read_text(),['vlan_dev_flow_offload_check']),(tnl,['tnl_info_decap_is_enable','tnl_info_encap_is_enable','tnl_info_decap_enable','tnl_info_encap_enable']),(ac,['mtk_npu_nf_hnat_tnl_info_preserve','mtk_npu_egress_matches','mtk_npu_egress_replace','mtk_npu_nf_hnat_tnl_info_setup','mtk_npu_bind_lock','mtk_npu_bind_unlock']),(core,['tnl_info_sta_init_no_tnl_lock','tnl_info_sta_is_init','tnl_info_sta_uninit_no_tnl_lock','mtk_npu_tnl_info_alloc_no_lock','mtk_npu_tnl_info_put_devices','mtk_npu_tnl_info_free_no_lock','mtk_npu_tnl_info_match_netdev_and_down','mtk_npu_tnl_offload_post_init','mtk_npu_tnl_offload']),((N/'npu_params.c').read_text(),['npu_transport_params_match','npu_network_params_match','mtk_npu_params_match'])]:
 for name in names:
  f=block(text,name);meta.append({'function':name,'sha256':hashlib.sha256(f.encode()).hexdigest()});funcs.append(f.replace('mtk_npu_tnl_info_alloc_no_lock(', 'real_alloc_no_lock(',1) if name=='mtk_npu_tnl_info_alloc_no_lock' else f)
if args.baseline_vlan:
 baseline=args.baseline_vlan.read_text()
else:
 with tempfile.TemporaryDirectory(prefix='tops-vlan-baseline-') as temp:
  copy=Path(temp)/'net/8021q/vlan_dev.c';copy.parent.mkdir(parents=True);copy.write_bytes((args.kernel/'net/8021q/vlan_dev.c').read_bytes())
  patch=R/'target/linux/mediatek/patches-6.6/9999-mtk-vlan-flow-offload-check-flag.patch'
  r=subprocess.run(['patch','--batch','--fuzz=0','-R','-p1','-d',temp],input=patch.read_bytes(),capture_output=True);assert not r.returncode,r.stderr
  baseline=block(copy.read_text(),'vlan_dev_flow_offload_check')
baseline=baseline.replace('vlan_dev_flow_offload_check(', 'baseline_vlan_check(',1)
baseline_lock=block((V/'fixtures/tnl-offload-irqsave-baseline.c').read_text(),'mtk_npu_tnl_offload').replace('mtk_npu_tnl_offload(', 'baseline_lock_offload(',1)
baseline_alloc=block((V/'fixtures/tnl-offload-before-allocation-rollback.c').read_text(),'mtk_npu_tnl_offload').replace('mtk_npu_tnl_offload(', 'baseline_alloc_offload(',1)
c=(V/'underlay-test-shim.h').read_text()+'\n'+types+dependency+'\n'+baseline+'\n'+'\n'.join(funcs)+baseline_lock+baseline_alloc+(V/'underlay-test-cases.c').read_text();path=O/'actual-underlay.c';path.write_text(c)
commands=[]
for argv in [['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(path),'-o',str(O/'actual-underlay')],[str(O/'actual-underlay'),'baseline'],[str(O/'actual-underlay'),'baseline-lock'],[str(O/'actual-underlay'),'baseline-allocation'],[str(O/'actual-underlay')],[str(O/'actual-underlay'),'required-ipv6']]:
 r=subprocess.run(argv,capture_output=True,text=True);commands.append({'argv':argv,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
 if len(commands)==1 and r.returncode:break
rec={'scope':'actual V3 FOE, firmware parameter and host context structures and source functions; neighbour, bitmap/hash primitives, kernel locks/IRQ/BH state, device refs and firmware worker/ack shimmed; no packet/hardware claim','functions':meta,'generated_c_sha256':hashlib.sha256(c.encode()).hexdigest(),'unmet_requirement':'IPv6 outer: equal IPv6 parameters are still rejected by the actual SDK matcher; required-ipv6 is an expected failure, not a passed IPv6 test.','commands':commands};(O/'receipt.json').write_text(json.dumps(rec,indent=2)+'\n');print(json.dumps({'commands':commands}));assert len(commands)==6 and [r['exit'] for r in commands]==[0,1,1,1,0,1]
