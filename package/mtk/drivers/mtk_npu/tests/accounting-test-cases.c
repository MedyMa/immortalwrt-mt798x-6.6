static unsigned int checks;
#define CHECK(x) do { ++checks;if(!(x)){fprintf(stderr,"check failed line %d: %s\n",__LINE__,#x);exit(1);} }while(0)
static struct mtk_hnat h;static struct hnat_account_ctx ctx;static struct hnat_slot slot;
static struct hnat_accounting total;static struct foe_entry live,prepared;
static unsigned long active_slot_bitmap;
static struct mtk_hnat_data data;static struct nf_conn inner,outer;
static bool test_baseline_sparse;
static void reset(void) {
 if(slot.owner)retire_locked(&ctx.ppe[0],&slot);
 reap_rcu();
 memset(&h,0,sizeof(h));memset(&ctx,0,sizeof(ctx));memset(&slot,0,sizeof(slot));
 memset(&total,0,sizeof(total));memset(&live,0,sizeof(live));memset(&prepared,0,sizeof(prepared));
 memset(&inner,0,sizeof(inner));memset(&outer,0,sizeof(outer));memset(registers,0,sizeof(registers));
 data.per_flow_accounting=true;data.version=MTK_HNAT_V3;
 h.account=&ctx;h.data=&data;h.ppe_num=h.foe_etry_num=1;h.nf_stat_en=true;
 h.ppe_base[0]=registers;h.acct[0]=&total;h.foe_table_cpu[0]=&live;hnat_priv=&h;
 ctx.ppe_num=ctx.slot_count=1;ctx.h=&h;ctx.ppe[0].h=&h;ctx.ppe[0].slots=&slot;
 active_slot_bitmap=0;ctx.ppe[0].poll_active=&active_slot_bitmap;
 live.bfib1.state=UNBIND;live.bfib1.pkt_type=IPV4_HNAPT;
 live.ipv4_hnapt.sip=0xc0000201;live.ipv4_hnapt.dip=0xc6336401;
 live.ipv4_hnapt.sport=12345;live.ipv4_hnapt.dport=443;
 prepared=live;prepared.bfib1.state=BIND;prepared.bfib1.ttl=1;
 prepared.bfib1.psn=1;prepared.bfib1.cah=1;prepared.bfib1.rmt=1;
 prepared.bfib1.vlan_layer=2;prepared.bfib1.time_stamp=123;
 prepared.ipv4_hnapt.new_sip=0xcb007101;prepared.ipv4_hnapt.new_dip=live.ipv4_hnapt.dip;
 inner.ct_general=outer.ct_general=1;mutation=0;mutation_ct=&inner;hw_bytes=hw_packets=0;fail_alloc=false;
}
static u64 prefill(void) {
 struct sk_buff skb={.ct=&inner,.info=IP_CT_NEW};bool accepted=false;
 CHECK(hnat_account_commit(&h,0,0,&skb,&prepared,true,false,0,&accepted));CHECK(accepted);
 CHECK(live.bfib1.state==UNBIND);CHECK(slot.owner&&slot.owner->ct==&inner);
 CHECK(!test_bit(0,ctx.ppe[0].poll_active));
 CHECK(slot.owner->prefill);CHECK(inner.ct_general==2);return slot.generation;
}
static bool confirm(u64 generation,struct nf_conn *ct,u8 info) {
 bool accepted=false;CHECK(hnat_account_tunnel_commit(&h,0,0,ct,info,&prepared,generation,&accepted));return accepted;
}
static bool confirm_unowned(u64 generation) {
 bool accepted=false;CHECK(hnat_account_commit(&h,0,0,NULL,&prepared,false,true,generation,&accepted));return accepted;
}
static void drain(u64 bytes,u64 packets) {
 struct hnat_accounting d;hw_bytes=bytes;hw_packets=packets;
 CHECK(!hnat_account_drain(&h,0,0,NULL,&d));CHECK(d.bytes==bytes&&d.packets==packets);
}
static void correct_owner_and_ledgers(void) {
 reset();u64 gen=prefill();CHECK(confirm(gen,&inner,IP_CT_NEW));
 CHECK(live.bfib1.state==BIND);CHECK(!slot.owner->prefill);
 CHECK(live.ipv4_hnapt.info_blk1==prepared.ipv4_hnapt.info_blk1);
 drain(1000,10);CHECK(total.bytes==1000&&slot.pending.bytes==1000&&slot.pending_ct.bytes==0);
 CHECK(inner.acct.counter[0].bytes.value==1000);
 CHECK(flush_ct_locked(&ctx.ppe[0],&slot));CHECK(inner.acct.counter[0].bytes.value==1000);
 CHECK(outer.acct.counter[0].bytes.value==0);CHECK(slot.pending.bytes==1000&&slot.pending_ct.bytes==0);
 CHECK(flush_ct_locked(&ctx.ppe[0],&slot));CHECK(inner.acct.counter[0].bytes.value==1000);
 struct sk_buff keepalive={.ct=&inner,.info=IP_CT_NEW};
 CHECK(hnat_account_publish_delta(&h,0,0,&keepalive));CHECK(keepalive.bytes==1000&&keepalive.packets==10);
 CHECK(!hnat_account_publish_delta(&h,0,0,&keepalive));CHECK(inner.acct.counter[0].bytes.value==1000);
 CHECK(slot.pending.bytes==0);drain(2000,20);h.entry_lock.held=true;
 CHECK(hnat_account_tunnel_invalidate(&h,0,0));h.entry_lock.held=false;
 CHECK(live.bfib1.state==INVALID);CHECK(slot.owner==NULL);
 CHECK(inner.acct.counter[0].bytes.value==3000&&inner.acct.counter[0].packets.value==30);
 CHECK(outer.acct.counter[0].bytes.value==0);CHECK(ctx.ppe[0].unknown_loss==1);
 reap_rcu();CHECK(inner.ct_general==1);
}
static void rejection_cases(void) {
 for(int action=1;action<=7;action++) {
  reset();u64 gen=prefill();mutation=action;
  CHECK(!confirm(gen,&inner,IP_CT_NEW));CHECK(live.bfib1.state!=BIND);
  CHECK(slot.owner==NULL);reap_rcu();CHECK(inner.ct_general==1);
 }
 reset();u64 gen=prefill();CHECK(!confirm(gen+1,&inner,IP_CT_NEW));CHECK(live.bfib1.state==UNBIND);
 reset();gen=prefill();CHECK(!confirm(gen,&outer,IP_CT_NEW));CHECK(live.bfib1.state==UNBIND);
 reset();gen=prefill();CHECK(!confirm(gen,&inner,IP_CT_IS_REPLY));CHECK(live.bfib1.state==UNBIND);
 reset();gen=prefill();inner.dying=true;CHECK(!confirm(gen,&inner,IP_CT_NEW));
 reset();gen=prefill();inner.templ=true;CHECK(!confirm(gen,&inner,IP_CT_NEW));
 reset();gen=prefill();inner.net.state.exiting=true;CHECK(!confirm(gen,&inner,IP_CT_NEW));
 reset();gen=prefill();ctx.ppe[0].paused=true;CHECK(!confirm(gen,&inner,IP_CT_NEW));CHECK(live.bfib1.state==UNBIND);
}
static void ownerless_and_failures(void) {
 reset();CHECK(confirm_unowned(0));CHECK(live.bfib1.state==BIND&&slot.owner==NULL);
 reset();CHECK(!confirm(0,NULL,IP_CT_NEW));CHECK(live.bfib1.state==UNBIND&&slot.owner==NULL);
 reset();fail_alloc=true;struct sk_buff skb={.ct=&inner,.info=IP_CT_NEW};bool accepted=false;
 CHECK(hnat_account_commit(&h,0,0,&skb,&prepared,true,false,0,&accepted));CHECK(accepted&&slot.owner==NULL);
 CHECK(!confirm(slot.generation,&inner,IP_CT_NEW));CHECK(live.bfib1.state==UNBIND);CHECK(inner.ct_general==1);
 for(int action=1;action<=5;action++) {reset();mutation=action;CHECK(!confirm_unowned(0));CHECK(live.bfib1.state!=BIND);}
 reset();u64 gen=prefill();CHECK(confirm(gen,&inner,IP_CT_NEW));inner.no_acct=true;
 drain(99,3);CHECK(!flush_ct_locked(&ctx.ppe[0],&slot));CHECK(slot.pending_ct.bytes==99);
 inner.no_acct=false;CHECK(flush_ct_locked(&ctx.ppe[0],&slot));CHECK(inner.acct.counter[0].bytes.value==99);
 reset();gen=prefill();CHECK(confirm(gen,&inner,IP_CT_NEW));h.nf_stat_en=false;
 drain(199,4);CHECK(!flush_ct_locked(&ctx.ppe[0],&slot));CHECK(slot.pending_ct.bytes==199);
 h.nf_stat_en=true;CHECK(flush_ct_locked(&ctx.ppe[0],&slot));CHECK(inner.acct.counter[0].bytes.value==199);
}
static void invalidate_races(void) {
 for(int action=1;action<=4;action++) {
  reset();u64 gen=prefill();CHECK(confirm(gen,&inner,IP_CT_NEW));mutation=action;
  h.entry_lock.held=true;CHECK(hnat_account_tunnel_invalidate(&h,0,0));h.entry_lock.held=false;
  CHECK(ctx.ppe[0].mismatch>0);
  if(action==1)CHECK(live.ipv4_hnapt.sip==0xc0000202&&live.bfib1.state==BIND);
  if(action==2)CHECK(live.bfib1.state==INVALID);
  if(action==3)CHECK(live.bfib1.state==FIN);
  if(action==4)CHECK(live.bfib1.pkt_type==IPV4_HNAT&&live.bfib1.state==BIND);
 }
}
static void sparse_polling(void) {
 reset();
 const unsigned count=8192;h.ppe_num=ctx.ppe_num=3;h.foe_etry_num=ctx.slot_count=count;
 for(unsigned p=0;p<3;p++) {
  ctx.ppe[p].h=&h;ctx.ppe[p].slots=calloc(count,sizeof(struct hnat_slot));
  ctx.ppe[p].poll_active=calloc((count+63)/64,sizeof(unsigned long));
  h.foe_table_cpu[p]=calloc(count,sizeof(struct foe_entry));
  h.acct[p]=calloc(count,sizeof(struct hnat_accounting));h.ppe_base[p]=poll_registers[p];
  CHECK(ctx.ppe[p].slots&&ctx.ppe[p].poll_active&&h.foe_table_cpu[p]&&h.acct[p]);
 }
 memset(poll_registers,0,sizeof(poll_registers));
 memset(poll_hw_bytes,0,sizeof(poll_hw_bytes));memset(poll_hw_packets,0,sizeof(poll_hw_packets));
 test_clock=test_clock_step=0;test_requeues=0;
 struct sk_buff skb={.ct=&inner,.info=IP_CT_NEW};bool accepted=false;
 /* A single active entry at the end of a three-PPE, 8192-slot table must
  * be read in the first invocation, once, without inspecting empty slots.
  */
 h.foe_table_cpu[0][8191]=live;
 CHECK(hnat_account_commit(&h,0,8191,&skb,&prepared,false,false,0,&accepted)&&accepted);
 CHECK(test_bit(8191,ctx.ppe[0].poll_active));
 poll_hw_bytes[0]=1200;poll_hw_packets[0]=12;
 if(test_baseline_sparse)baseline_account_poll(&ctx.poll.work);else account_poll(&ctx.poll.work);
 CHECK(ctx.ppe[0].poll_scanned==1&&ctx.ppe[0].poll_reads==1);
 CHECK(ctx.ppe[1].poll_scanned==0&&ctx.ppe[2].poll_scanned==0);
 CHECK(inner.acct.counter[0].bytes.value==1200);
 CHECK(ctx.ppe[0].slots[8191].pending_ct.bytes==0);
 CHECK(ctx.ppe[0].slots[8191].pending.bytes==1200);
 /* Exercise circular cursors and multiple owned entries: no owner can
  * consume the entire budget by being reread after the bitmap wraps.
  */
 for(unsigned p=0;p<3;p++)for(unsigned n=0;n<2;n++) {
  unsigned at=n?31:8000;h.foe_table_cpu[p][at]=live;
  CHECK(hnat_account_commit(&h,p,at,&skb,&prepared,false,false,0,&accepted)&&accepted);
 }
 for(unsigned p=0;p<3;p++)ctx.ppe[p].poll_cursor=32;
 u64 old_reads[3];for(unsigned p=0;p<3;p++)old_reads[p]=ctx.ppe[p].poll_reads;
 account_poll(&ctx.poll.work);
 CHECK(ctx.ppe[0].poll_reads-old_reads[0]==3);
 CHECK(ctx.ppe[1].poll_reads-old_reads[1]==2&&ctx.ppe[2].poll_reads-old_reads[2]==2);
 /* With budget one, all three PPEs make progress across invocations. */
 account_poll_budget=1;
 for(unsigned p=0;p<3;p++)old_reads[p]=ctx.ppe[p].poll_reads;
 for(unsigned n=0;n<3;n++)account_poll(&ctx.poll.work);
 for(unsigned p=0;p<3;p++)CHECK(ctx.ppe[p].poll_reads-old_reads[p]==1);
 account_poll_budget=64;
 /* The service-time guard must prevent a transaction after the deadline. */
 for(unsigned p=0;p<3;p++)old_reads[p]=ctx.ppe[p].poll_reads;
 test_clock=0;test_clock_step=HNAT_ACCOUNT_POLL_TIME_NS+1;
 account_poll(&ctx.poll.work);test_clock_step=0;
 for(unsigned p=0;p<3;p++)CHECK(ctx.ppe[p].poll_reads==old_reads[p]);
 /* Contention does not spin, duplicate a hardware read or consume data. */
 ctx.ppe[0].lock.held=true;poll_hw_bytes[0]=300;
 account_poll(&ctx.poll.work);ctx.ppe[0].lock.held=false;
 CHECK(ctx.poll_lock_skips[0].value>0&&poll_hw_bytes[0]==300);
 poll_hw_bytes[0]=0;
 for(unsigned p=0;p<3;p++)for(unsigned at=0;at<count;at++)
  if(ctx.ppe[p].slots[at].owner)retire_locked(&ctx.ppe[p],&ctx.ppe[p].slots[at]);
 reap_rcu();CHECK(inner.ct_general==1);
 for(unsigned p=0;p<3;p++)old_reads[p]=ctx.ppe[p].poll_reads;
 account_poll(&ctx.poll.work);
 for(unsigned p=0;p<3;p++)CHECK(ctx.ppe[p].poll_reads==old_reads[p]);
 for(unsigned p=0;p<3;p++) {
  free(ctx.ppe[p].slots);free(ctx.ppe[p].poll_active);
  free(h.foe_table_cpu[p]);free(h.acct[p]);
 }
 reset();
}
int main(int argc,char **argv) {
 if(argc>1&&!strcmp(argv[1],"baseline-sparse")){test_baseline_sparse=true;sparse_polling();return 0;}
 correct_owner_and_ledgers();rejection_cases();ownerless_and_failures();invalidate_races();sparse_polling();reset();reap_rcu();
 printf("PASS %u assertions: actual accounting commit/drain/ledger/publish/invalidate and sparse active-bitmap poller; MMIO, CT and worker scheduling shimmed\n",checks);return 0;
}
