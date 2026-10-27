#!/usr/bin/env python3
"""Compile candidate accounting functions verbatim; shim only kernel/MMIO APIs."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
V=Path(__file__).resolve().parent
P=Path(__file__).resolve().parents[5]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,required=True,help='external directory for generated C, binary and receipt')
args=parser.parse_args()
H=P/'target/linux/mediatek/files-6.6/drivers/net/ethernet/mediatek/mtk_hnat'
O=args.output.resolve();O.mkdir(parents=True,exist_ok=True)
def extract(s,name,kind='function'):
    if kind=='struct':
        start=s.index('struct '+name+' {');brace=s.index('{',start)
    else:
        m=re.search(r'(?m)^(?:static\s+)?(?:bool|void|int)\s+'+re.escape(name)+r'\s*\(',s)
        if not m:raise ValueError(name)
        start=m.start();brace=s.index('{',m.end())
    mask=re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',lambda m:' '*len(m[0]),s,flags=re.S)
    depth=0
    for at in range(brace,len(s)):
        if mask[at]=='{':depth+=1
        elif mask[at]=='}':
            depth-=1
            if not depth:return s[start:at+1]+(';' if kind=='struct' else '')+'\n'
    raise ValueError(name)
header=(H/'hnat.h').read_text();account=(H/'hnat_account.c').read_text();hook=(H/'hnat_nf_hook.c').read_text()
start=header.index('#if defined(CONFIG_MEDIATEK_NETSYS_V2) || defined(CONFIG_MEDIATEK_NETSYS_V3)',header.index('/* Descriptor Structure */'))
enums=header[header.index('enum FoeEntryState'):header.index('/* Common Definition*/')].split('/*--------------------------------------------------------------------------*/')[0]
structs=enums+'\n'+header[start:header.index('struct hnat_flow_entry {',start)]
structs+='\n'+extract(header,'hnat_accounting','struct')
for name in ['hnat_owner','hnat_slot','hnat_account_ppe','hnat_account_ctx']:
    s=extract(account,name,'struct')
    # Only the baseline poller needs its retired scan cursor. This host-only
    # compatibility field does not change production layout or poll behavior.
    if name=='hnat_account_ctx':s=s.replace('struct hnat_account_ctx {','struct hnat_account_ctx {\n u32 poll_slot; /* baseline harness only */')
    structs+='\n'+s
dependencies=r'''
struct mtk_hnat_data { bool per_flow_accounting;int version; };
struct mtk_hnat { struct hnat_account_ctx *account;struct mtk_hnat_data *data;
 u32 ppe_num,foe_etry_num;void *ppe_base[MAX_PPE_NUM];struct hnat_accounting *acct[MAX_PPE_NUM];
 struct foe_entry *foe_table_cpu[MAX_PPE_NUM];spinlock_t entry_lock;bool nf_stat_en; };
static struct mtk_hnat *hnat_priv;
#define PPE_MIB_SER_CR 0
#define PPE_MIB_SER_R0 4
#define PPE_MIB_SER_R1 8
#define PPE_MIB_SER_R2 12
#define PPE_MIB_SER_R3 16
#define BIT_MIB_BUSY BIT(16)
static u32 registers[5];static u64 hw_bytes,hw_packets;static int mutation;
static u32 poll_registers[MAX_PPE_NUM][5];
static u64 poll_hw_bytes[MAX_PPE_NUM],poll_hw_packets[MAX_PPE_NUM];
static struct nf_conn *mutation_ct;
static u32 readl(void *address) { return *(u32 *)address; }
static void writel(u32 val,void *address) {
 *(u32 *)address=val;
 if(address==registers) {registers[1]=(u32)hw_bytes;registers[2]=hw_bytes>>32;
 registers[3]=(u32)hw_packets;registers[4]=hw_packets>>32;hw_bytes=hw_packets=0;}
 for(unsigned i=0;i<MAX_PPE_NUM;i++)if(address==poll_registers[i]) {
  poll_registers[i][1]=(u32)poll_hw_bytes[i];poll_registers[i][2]=poll_hw_bytes[i]>>32;
  poll_registers[i][3]=(u32)poll_hw_packets[i];poll_registers[i][4]=poll_hw_packets[i]>>32;
  poll_hw_bytes[i]=poll_hw_packets[i]=0;
 }
}
static int shim_poll(void *base) {
 struct foe_entry *e=&hnat_priv->foe_table_cpu[0][0];
 if(mutation==1)e->ipv4_hnapt.sip++;
 if(mutation==2)e->bfib1.state=INVALID;
 if(mutation==3)e->bfib1.state=FIN;
 if(mutation==4)e->bfib1.pkt_type=IPV4_HNAT;
 if(mutation==5)hnat_priv->account->ppe[0].slots[0].generation++;
 if(mutation==6&&mutation_ct)mutation_ct->dying=true;
 if(mutation==7)return -ETIMEDOUT;
 *(u32 *)base&=~BIT_MIB_BUSY;return 0;
}
#define readx_poll_timeout_atomic(op,addr,val,cond,delay,timeout) ({int ret=shim_poll(addr);(val)=op(addr);ret;})
'''
names_hook=['hnat_flow_entry_format_compatible','hnat_flow_entry_match','hnat_flow_entry_unbind_match']
names_account=['account_valid','account_identity','owner_free','flush_ct_locked','retire_locked','owner_live','drain_locked','hnat_account_drain','account_commit','hnat_account_commit','hnat_account_tunnel_commit','hnat_account_publish_delta','hnat_account_tunnel_invalidate','account_poll']
funcs=[extract(hook,n) for n in names_hook]+[extract(account,n) for n in names_account]
baseline_revision='1ea2dd3a50e166445d8997211840f519285dd114'
baseline_source=(V/'fixtures/account-poll-scan-baseline.c').read_text()
baseline_poll=extract(baseline_source,'account_poll').replace('account_poll(', 'baseline_account_poll(',1)
source=(V/'accounting-test-shim.h').read_text()+'\n'+structs+'\n'+dependencies+'\n'+(H/'hnat_account_ledger.h').read_text()+'\n'+'\n'.join(funcs)+'\n'+baseline_poll+'\n'+(V/'accounting-test-cases.c').read_text()
c=O/'actual-accounting.c';c.write_text(source)
commands=[]
for label,args in [('compile',['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function','-g','-O1','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(c),'-o',str(O/'actual-accounting')]),('baseline-sparse',[str(O/'actual-accounting'),'baseline-sparse']),('run',[str(O/'actual-accounting')])]:
    r=subprocess.run(args,capture_output=True,text=True);(O/(label+'.log')).write_text(r.stdout+r.stderr)
    commands.append({'argv':args,'exit':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
    if label=='baseline-sparse':
        if r.returncode!=1 or 'poll_scanned==1&&ctx.ppe[0].poll_reads==1' not in r.stderr:break
    elif r.returncode:break
receipt={'kind':'verbatim candidate functions and actual V3 FOE/owner/slot/PPE layouts; kernel, CT and MMIO dependencies shimmed, not hardware proof','source_base':subprocess.check_output(['git','-C',str(P),'rev-parse','HEAD'],text=True).strip(),'sources':{n:hashlib.sha256((H/n).read_bytes()).hexdigest() for n in ['hnat.h','hnat_nf_hook.c','hnat_account.c','hnat_account_ledger.h']},'extracted_functions':[{'name':n,'sha256':hashlib.sha256(f.encode()).hexdigest()} for n,f in zip(names_hook+names_account,funcs)],'test_sha256':hashlib.sha256(c.read_bytes()).hexdigest(),'commands':commands}
receipt['baseline_sparse_revision']=baseline_revision
(O/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps({'commands':commands,'test_sha256':receipt['test_sha256']},ensure_ascii=False));raise SystemExit(0 if len(commands)==3 and [r['exit'] for r in commands]==[0,1,0] else 1)
