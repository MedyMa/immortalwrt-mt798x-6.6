/* SPDX-License-Identifier: GPL-2.0
 * Host boundary fixture. Driver structures/functions are inserted verbatim.
 * Kernel primitives, MMIO values and deferred RCU scheduling are modeled.
 * The simplified FOE layout is not a hardware descriptor ABI validation.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef struct { u64 value; } atomic64_t;
typedef struct { bool held; } spinlock_t;
struct delayed_work { int unused; };
struct rcu_head { struct rcu_head *next; void (*callback)(struct rcu_head *); };
struct hlist_node { struct hlist_node *next, **pprev; };
struct hlist_head { struct hlist_node *first; };
#define container_of(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
#define hlist_for_each_entry_safe(p,n,h,m) \
 for (struct hlist_node *_cur=(h)->first; _cur && ((p)=container_of(_cur,__typeof__(*(p)),m), (n)=_cur->next,1); _cur=(n))
static void hlist_del_init(struct hlist_node *p) { if(p->pprev){*p->pprev=p->next;if(p->next)p->next->pprev=p->pprev;p->pprev=NULL;p->next=NULL;} }
static void hlist_add_head(struct hlist_node *p,struct hlist_head *h){p->next=h->first;if(p->next)p->next->pprev=&p->next;h->first=p;p->pprev=&h->first;}
#define CONFIG_MEDIATEK_NETSYS_V3 1
#define MAX_PPE_NUM 3
#define CFG_PPE_NUM 3
#define SLOTS 8
#define BIT(n) (1U<<(n))
#define GENMASK(h,l) ((u32)((~0ULL<<(l))&(~0ULL>>(63-(h)))))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define dma_wmb() ((void)0)
#define wmb() ((void)0)
#define min(a,b) ((a)<(b)?(a):(b))
#define min_t(t,a,b) ((t)min(a,b))
#define GFP_ATOMIC 0
#define __iomem
#define IS_IPV4_HNAPT(p) ((p)->bfib1.pkt_type==0)
#define IS_IPV4_HNAT(p) ((p)->bfib1.pkt_type==1)
#define IS_IPV6_5T_ROUTE(p) ((p)->bfib1.pkt_type==5)
#define IS_IPV6_HNAPT(p) ((p)->bfib1.pkt_type==18)
#define IS_IPV6_HNAT(p) ((p)->bfib1.pkt_type==19)
#define IS_IPV4_GRP(p) (IS_IPV4_HNAPT(p)||IS_IPV4_HNAT(p))
#define UDF_PINGPONG_IFIDX 0xffU
#define PPE_MIB_SER_CR 0
#define PPE_MIB_SER_R0 4
#define PPE_MIB_SER_R1 8
#define PPE_MIB_SER_R2 12
#define PPE_MIB_SER_R3 16
#define PPE_TB_CFG 20
#define BIT_MIB_BUSY BIT(31)
#define SMA 3
#define SMA_ONLY_FWD_CPU 1
#define MTK_HNAT_V3 3
#define HZ 100
#define INIT_HLIST_NODE(p) do { (p)->next=NULL; (p)->pprev=NULL; } while (0)
#define hnat_diag_bind(...) ((void)0)
enum { INVALID,UNBIND,BIND };
enum ip_conntrack_info { IP_CT_ESTABLISHED, IP_CT_IS_REPLY };
#define CTINFO2DIR(i) ((unsigned int)(i))
#define IP_CT_DIR_MAX 2
struct foe_info {u32 time_stamp:8,unused:15,pkt_type:5,state:2,udp:1,sta:1;};
struct foe_fields {
 union { struct foe_info udib1,bfib1;u32 info_blk1; };
 u32 sip,dip;u16 sport,dport;u32 new_sip,new_dip;
 struct {u32 dscp;} iblk2;
 u32 m_timestamp,act_dp,tops_entry;
};
struct foe_entry {
 union {struct foe_info udib1,bfib1;u32 info_blk1;struct foe_fields ipv4_hnapt,ipv6_5t_route;};
};
struct hnat_accounting {u64 bytes,packets;};
struct nf_conntrack_zone {u16 id;u8 dir,flags;};
struct net {bool exiting;};
struct nf_conn_counter {atomic64_t bytes,packets;};
struct nf_conn_acct {struct nf_conn_counter counter[2];};
struct nf_conntrack {int refs;};
struct nf_conn {struct nf_conntrack ct_general;struct net *net;struct nf_conntrack_zone zone;struct nf_conn_acct acct;bool dying,template;};
struct sk_buff {struct nf_conn *ct;enum ip_conntrack_info dir;};
struct net_device {int refs,ifindex;};
struct hnat_account_net {bool exiting;};
static unsigned int account_net_id;
struct mtk_hnat_data {bool per_flow_accounting;int version;};
struct hnat_account_ctx;
struct mtk_hnat {
 struct mtk_hnat_data *data;struct hnat_account_ctx *account;
 u32 ppe_num,foe_etry_num,nf_stat_en;void *ppe_base[3],*fe_base;
 struct foe_entry *foe_table_cpu[3];struct hnat_accounting *acct[3];
 struct hlist_head *foe_flow[3];spinlock_t entry_lock,flow_entry_lock;
 int hnat_sma_build_entry_timer;
};
/* @PRODUCTION_TYPES@ */
#include "hnat_account_ledger.h"
static struct mtk_hnat *hnat_priv;
void hnat_flow_entry_clear(u32 ppe_id);
static struct rcu_head *rcu_queue;
static int lock_depth,ct_puts,dev_puts,owner_allocs,owner_frees,cache_flushes;
static unsigned long jiffies=100;
static atomic64_t hnat_flow_entry_sequence;
static u64 atomic64_inc_return(atomic64_t *a){return ++a->value;}
static u64 ktime_get(void){return jiffies*10000000ULL;}
static u64 ktime_to_ms(u64 value){return value/1000000;}
static void (*unlock_hook)(void);
static bool allocation_fail;
static void spin_lock_bh(spinlock_t *p){assert(!p->held);p->held=true;lock_depth++;}
static void spin_unlock_bh(spinlock_t *p){assert(p->held);p->held=false;lock_depth--;if(!lock_depth&&unlock_hook){void(*f)(void)=unlock_hook;unlock_hook=NULL;f();}}
#define lockdep_assert_held(p) assert((p)->held)
static void atomic64_add(u64 n,atomic64_t *a){a->value+=n;}
static void __clear_bit(unsigned long n,unsigned long *p){*p&=~(1UL<<n);}
static void __set_bit(unsigned long n,unsigned long *p){*p|=1UL<<n;}
static void *kzalloc(size_t n,int flags){(void)flags;if(allocation_fail)return NULL;owner_allocs++;return calloc(1,n);}
static void kfree(void *p){if(p)owner_frees++;free(p);}
static struct nf_conn *nf_ct_get(struct sk_buff *s,enum ip_conntrack_info *i){*i=s->dir;return s->ct;}
static struct nf_conn_acct *nf_conn_acct_find(struct nf_conn *ct){return &ct->acct;}
static bool nf_ct_is_dying(struct nf_conn *ct){return ct->dying;}
static bool nf_ct_is_template(struct nf_conn *ct){return ct->template;}
static struct net *nf_ct_net(struct nf_conn *ct){return ct->net;}
static struct nf_conntrack_zone *nf_ct_zone(struct nf_conn *ct){return &ct->zone;}
static void *net_generic(struct net *n,unsigned int id){(void)id;return n;}
static void nf_conntrack_get(struct nf_conntrack *n){assert(n->refs>0);n->refs++;}
static void nf_ct_put(struct nf_conn *ct){assert(!lock_depth&&ct->ct_general.refs>1);ct->ct_general.refs--;ct_puts++;}
static void dev_put(struct net_device *dev){assert(!lock_depth&&dev->refs>1);dev->refs--;dev_puts++;}
static void dev_hold(struct net_device *dev){assert(dev->refs>0);dev->refs++;}
static void call_rcu(struct rcu_head *head,void(*f)(struct rcu_head*)){head->callback=f;head->next=rcu_queue;rcu_queue=head;}
static void flush_rcu(void){assert(!lock_depth);while(rcu_queue){struct rcu_head *p=rcu_queue;rcu_queue=p->next;p->callback(p);}}
static bool hnat_flow_entry_unbind_match(const struct foe_entry *a,const struct foe_entry *b){return b->bfib1.state==UNBIND&&a->bfib1.pkt_type==b->bfib1.pkt_type&&a->bfib1.udp==b->bfib1.udp&&a->ipv4_hnapt.sip==b->ipv4_hnapt.sip&&a->ipv4_hnapt.dip==b->ipv4_hnapt.dip&&a->ipv4_hnapt.sport==b->ipv4_hnapt.sport&&a->ipv4_hnapt.dport==b->ipv4_hnapt.dport;}
static bool hnat_flow_entry_match(const struct foe_entry *a,const struct foe_entry *b){struct foe_entry unbound=*b;unbound.bfib1.state=UNBIND;return hnat_flow_entry_unbind_match(a,&unbound);}
static u32 regs[3][16],fe_regs[16];
static struct hnat_accounting hardware[3][SLOTS];
static int polls,read_mode; /* 0=complete, 1=timeout, 2=identity changed while reading */
static u32 *pending_reg;static u32 pending_slot;
static u32 readl(void *p){return *(u32*)p;}
static int reg_ppe(void *p){for(int i=0;i<3;i++)if((char*)p>=(char*)regs[i]&&(char*)p<(char*)(regs[i]+16))return i;abort();}
static void writel(u32 value,void *p){*(u32*)p=value;int i=reg_ppe(p);if(p==regs[i]){pending_reg=p;pending_slot=value&0xffff;}}
static int model_poll(void *p,u32 *value){
 int i=reg_ppe(p);assert(pending_reg==p&&pending_slot<SLOTS);polls++;
 if(read_mode==1){*(u32*)p=BIT_MIB_BUSY;*value=BIT_MIB_BUSY;return -ETIMEDOUT;}
 struct hnat_accounting d=hardware[i][pending_slot];hardware[i][pending_slot]=(struct hnat_accounting){0};
 regs[i][1]=(u32)d.bytes;regs[i][2]=d.bytes>>32;regs[i][3]=(u32)d.packets;regs[i][4]=d.packets>>32;
 *(u32*)p=0;*value=0;
 if(read_mode==2)hnat_priv->foe_table_cpu[i][pending_slot].ipv4_hnapt.dip++;
 return 0;
}
#define readx_poll_timeout_atomic(op,p,val,cond,delay,timeout) model_poll(p,&(val))
static void cr_set_field(void *p,int mask,int value){(void)p;(void)mask;(void)value;}
static void hnat_cache_ebl(int enable){assert(enable==1);cache_flushes++;}
static void mod_timer(int *p,unsigned long expiry){*p=(int)expiry;}
/* @PRODUCTION_FUNCTIONS@ */
static struct mtk_hnat h;
static struct mtk_hnat_data soc;
static struct hnat_account_ctx ctx;
static struct foe_entry tables[3][SLOTS];
static struct hnat_accounting totals[3][SLOTS];
static struct hnat_slot slots[3][SLOTS];
static struct hlist_head shadows[3][SLOTS/4];
static unsigned long active[3];
static struct net network;
static struct nf_conn ct[4];
static struct net_device tunnel_dev;
static int checks,failures;
static void check(bool value,const char *name){checks++;printf("%s %s\n",value?"PASS":"FAIL",name);if(!value)failures++;}
static void setup(void){
 flush_rcu();
 memset(&h,0,sizeof(h));memset(&ctx,0,sizeof(ctx));memset(tables,0,sizeof(tables));memset(totals,0,sizeof(totals));memset(slots,0,sizeof(slots));memset(shadows,0,sizeof(shadows));memset(active,0,sizeof(active));memset(regs,0,sizeof(regs));memset(hardware,0,sizeof(hardware));memset(ct,0,sizeof(ct));
 soc=(struct mtk_hnat_data){true,MTK_HNAT_V3};h.data=&soc;h.account=&ctx;h.ppe_num=3;h.foe_etry_num=SLOTS;h.nf_stat_en=1;h.fe_base=fe_regs;ctx.h=&h;ctx.ppe_num=3;ctx.slot_count=SLOTS;hnat_priv=&h;
 for(int i=0;i<3;i++){h.ppe_base[i]=regs[i];h.foe_table_cpu[i]=tables[i];h.acct[i]=totals[i];h.foe_flow[i]=shadows[i];ctx.ppe[i].h=&h;ctx.ppe[i].slots=slots[i];ctx.ppe[i].poll_active=&active[i];}
 for(int i=0;i<4;i++){ct[i].ct_general.refs=1;ct[i].net=&network;}
 tunnel_dev=(struct net_device){.refs=1,.ifindex=88};network.exiting=false;
 read_mode=0;polls=0;pending_reg=NULL;unlock_hook=NULL;allocation_fail=false;ct_puts=dev_puts=0;
}
static struct foe_entry prepared(u32 key){struct foe_entry e={0};e.bfib1.state=BIND;e.ipv4_hnapt.sip=key;e.ipv4_hnapt.dip=2;e.ipv4_hnapt.sport=1234;e.ipv4_hnapt.dport=443;return e;}
static bool bind(unsigned int slot,unsigned int conn,bool prefill){
 struct foe_entry e=prepared(conn+1);tables[0][slot]=e;tables[0][slot].bfib1.state=UNBIND;struct sk_buff skb={&ct[conn],IP_CT_ESTABLISHED};bool accepted=false;
 bool handled=hnat_account_commit(&h,0,slot,&skb,&e,prefill,false,0,&accepted);assert(handled&&accepted);return accepted;
}
static bool confirm(unsigned int slot,unsigned int conn,u64 generation){struct foe_entry e=prepared(conn+1);struct sk_buff skb={&ct[conn],IP_CT_ESTABLISHED};bool accepted=false;bool handled=hnat_account_commit(&h,0,slot,&skb,&e,false,true,generation,&accepted);assert(handled);return accepted;}
static void add_shadow(unsigned int slot,unsigned int conn){struct hnat_flow_entry *f=calloc(1,sizeof(*f));owner_allocs++;f->hash=slot;f->ppe_index=0;f->tunnel_ct=&ct[conn];f->tunnel_dev=&tunnel_dev;nf_conntrack_get(&ct[conn].ct_general);tunnel_dev.refs++;hlist_add_head(&f->list,&shadows[0][slot/4]);}
static void clean(void){unlock_hook=NULL;for(int i=0;i<3;i++){hnat_account_pause(&h,i);hnat_flow_entry_clear(i);}flush_rcu();for(int i=0;i<4;i++)assert(ct[i].ct_general.refs==1);assert(tunnel_dev.refs==1&&!lock_depth);}
static void during_clear_confirm(void){
#ifdef TEST_HAS_CLEARING
 if(ctx.ppe[0].clearing)check(!confirm(1,1,slots[0][1].generation),"publication is blocked while bulk clear is in progress");
#endif
}
static void invalidate_after_snapshot(void){hnat_account_invalidate(&h,0,0);}
static void arm_snapshot_invalidation(void){unlock_hook=invalidate_after_snapshot;}
int main(void){
 setup();bind(0,0,false);hardware[0][0]=(struct hnat_accounting){4096,7};u64 gen=slots[0][0].generation;
 foe_clear_all_bind_entries_body();
 check(tables[0][0].bfib1.state==INVALID&&!slots[0][0].owner&&slots[0][0].generation>gen,"GOING_DOWN clear retires the bound owner and generation");
 check(ct[0].acct.counter[0].bytes.value==4096&&ct[0].acct.counter[0].packets.value==7,"verified final MIB delta reaches the original CT once");
 check(ct[0].ct_general.refs==2,"old CT reference remains pinned until RCU callback");flush_rcu();check(ct[0].ct_general.refs==1,"old CT reference is released outside the PPE lock");
 foe_clear_all_bind_entries_body();check(ct[0].acct.counter[0].bytes.value==4096,"repeated clear does not duplicate the final delta");clean();

 setup();bind(0,0,false);bind(1,1,true);add_shadow(1,1);gen=slots[0][1].generation;
 unlock_hook=during_clear_confirm;foe_clear_all_bind_entries_body();
 check(!slots[0][1].owner&&slots[0][1].generation>gen,"GOING_DOWN also retires an UNBIND software prefill epoch");
 check(shadows[0][0].first==NULL,"bulk clear removes the cached tunnel/WHNAT shadow");
 check(!confirm(1,1,gen),"an old prefill cannot publish BIND after GOING_DOWN");flush_rcu();
 check(ct[1].ct_general.refs==1&&tunnel_dev.refs==1,"prefill CT and tunnel-device references retire together");clean();

 setup();bind(1,0,true);gen=slots[0][1].generation;foe_clear_all_bind_entries_body();
 check(!confirm(1,0,gen),"GOING_DOWN rejects the retired prefill generation");
 bind(1,2,true);u64 rebind_gen=slots[0][1].generation;
 check(rebind_gen>gen&&confirm(1,2,rebind_gen),"a fresh WiFi prefill binds after the clear fence opens");clean();

 setup();bind(1,1,true);gen=slots[0][1].generation;
 foe_clear_all_bind_entries_body();flush_rcu();
 struct foe_entry stale=prepared(2);tables[0][1]=stale;tables[0][1].bfib1.state=UNBIND;
 check(!hnat_flow_entry_store(&stale,0,1,gen,&tunnel_dev,&ct[1],IP_CT_ESTABLISHED),"a delayed shadow store cannot republish the retired generation into a relearned slot");
 flush_rcu();check(shadows[0][0].first==NULL&&ct[1].ct_general.refs==1&&tunnel_dev.refs==1,"a rejected old shadow releases its CT and device references");clean();

 setup();bind(0,0,false);unlock_hook=arm_snapshot_invalidation;
 foe_clear_all_bind_entries_body();
 check(tables[0][0].bfib1.state==INVALID&&!slots[0][0].owner,"concurrent owner retirement cannot make bulk clear skip a bound descriptor");clean();

 setup();bind(0,0,true);gen=slots[0][0].generation;hnat_account_pause(&h,0);
 check(!confirm(0,0,gen),"paused accounting refuses in-flight publication");
 check(!hnat_account_generation_snapshot(&h,0,0,&gen),"paused PPE does not issue a generation token");
 hnat_account_reset_table(&h,0);hnat_flow_entry_clear(0);hnat_account_resume(&h,0);
 bind(0,1,true);u64 new_gen=slots[0][0].generation;
 check(new_gen>gen&&!confirm(0,1,gen),"reset rejects an old token after slot reuse");
 check(confirm(0,1,new_gen),"a new prefill can bind after resume");
 hardware[0][0]=(struct hnat_accounting){123,2};hnat_account_pause(&h,0);
 check(ct[1].acct.counter[0].bytes.value==123&&ct[0].acct.counter[0].bytes.value==0,"post-reset bytes stay with the new CT");clean();

 setup();bind(0,0,false);gen=slots[0][0].generation;hnat_account_clear(&h,0,0,true);bind(0,1,false);u64 g2=slots[0][0].generation;
 hnat_account_clear_if_generation(&h,0,0,false,gen);
 check(slots[0][0].owner&&slots[0][0].owner->ct==&ct[1]&&slots[0][0].generation==g2&&tables[0][0].bfib1.state==BIND,"a stale conditional clear cannot retire a newer owner");clean();

 setup();bind(0,0,false);hardware[0][0]=(struct hnat_accounting){987,3};read_mode=2;hnat_account_clear(&h,0,0,false);
 check(ct[0].acct.counter[0].bytes.value==0&&ctx.ppe[0].discarded_bytes==987,"a changed hardware identity is not attributed to the old CT");read_mode=0;clean();

 setup();bind(0,0,false);hardware[0][0]=(struct hnat_accounting){555,1};read_mode=1;hnat_account_clear(&h,0,0,false);
 check(ctx.ppe[0].final_errors==1&&ctx.ppe[0].quarantined&&!slots[0][0].owner,"MIB timeout quarantines the transaction and retires the owner");
 check(ct[0].acct.counter[0].bytes.value==0&&ctx.ppe[0].unknown_loss>0,"unread tail bytes stay unknown rather than fabricated");regs[0][0]=0;read_mode=0;clean();

 setup();bind(0,0,false);hardware[0][0]=(struct hnat_accounting){777,4};ct[0].dying=true;hnat_account_pause(&h,0);
 check(ct[0].acct.counter[0].bytes.value==777&&!slots[0][0].owner,"a dying but pinned CT receives its verified final delta");clean();

 setup();bind(0,0,true);hnat_account_pause(&h,0);foe_clear_all_bind_entries_body();
 check(ctx.ppe[0].paused,"clearing cannot resume an independently paused PPE");clean();
 check(owner_allocs==owner_frees,"all allocated owner and shadow objects were released");
 printf("{\"checks\":%d,\"failures\":%d,\"hardware_test\":false}\n",checks,failures);
 return failures?1:0;
}
