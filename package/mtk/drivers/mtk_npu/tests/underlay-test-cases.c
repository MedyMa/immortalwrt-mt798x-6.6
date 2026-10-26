static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static struct net_device physical,ppp,vlan,tunnel,other;
static const struct net_device_ops physical_ops;
static void init_devices(void){
 memset(&physical,0,sizeof(physical));physical.running=physical.physical=true;physical.net=&init_net;memset(physical.dev_addr,0x20,6);
 physical.netdev_ops=&physical_ops;
 ppp=physical;ppp.physical=false;vlan=ppp;vlan.vlan=true;vlan.real=&physical;vlan.vid=35;vlan.qos=0xa000;
 tunnel=ppp;other=physical;other.physical=false;memset(neighbour.ha,0x40,6);neighbour.nud_state=1;have_neighbour=true;arp_reads=releases=0;
}
static struct hnat_tunnel_token path_case(unsigned flags){
 struct sk_buff s={.priority=5};struct flow_offload_hw_path path={.dev=&physical,.virt_dev=flags&BIT(DEV_PATH_VLAN)?&vlan:&physical,.flags=flags,.vlan_proto=htons(ETH_P_8021Q),.vlan_id=35,.pppoe_sid=123};
 struct hnat_tunnel_token token={0};const struct net_device *u=flags&BIT(DEV_PATH_PPPOE)?&ppp:flags&BIT(DEV_PATH_VLAN)?&vlan:&physical;
 CHECK(hnat_tunnel_outer_path(&s,u,&path,&token));CHECK(token.egress==&physical&&token.underlay==u);
 struct foe_entry e={0};e.bfib1.state=2;e.bfib1.ttl=1;e.bfib1.cah=1;e.bfib1.rmt=1;e.bfib1.time_stamp=33;e.ipv4_hnapt.new_sip=55;
 hnat_tunnel_outer_l2(&e,&token);CHECK(e.bfib1.psn==!!(flags&BIT(DEV_PATH_PPPOE)));CHECK(e.bfib1.vlan_layer==!!(flags&BIT(DEV_PATH_VLAN)));
 CHECK(e.ipv4_hnapt.vlan1==(flags&BIT(DEV_PATH_VLAN)?0xa023:0));CHECK(e.ipv4_hnapt.pppoe_id==(flags&BIT(DEV_PATH_PPPOE)?123:0));
 CHECK(e.bfib1.ttl&&e.bfib1.cah&&e.bfib1.rmt&&e.bfib1.state==2&&e.bfib1.time_stamp==33&&e.ipv4_hnapt.new_sip==55);
 return token;
}
static void path_rejections(void){
 struct sk_buff s={0};struct hnat_tunnel_token t={0};struct flow_offload_hw_path p={.dev=&physical,.virt_dev=&vlan,.flags=BIT(DEV_PATH_VLAN),.vlan_proto=htons(ETH_P_8021Q),.vlan_id=35};
 p.flags|=BIT(DEV_PATH_DSA);CHECK(!hnat_tunnel_outer_path(&s,&vlan,&p,&t));p.flags=BIT(DEV_PATH_VLAN);
 p.vlan_proto=htons(ETH_P_8021AD);CHECK(!hnat_tunnel_outer_path(&s,&vlan,&p,&t));p.vlan_proto=htons(ETH_P_8021Q);
 p.vlan_id=4095;CHECK(!hnat_tunnel_outer_path(&s,&vlan,&p,&t));p.vlan_id=35;
 vlan.real=&other;CHECK(!hnat_tunnel_outer_path(&s,&vlan,&p,&t));vlan.real=&physical;
 p.flags=BIT(DEV_PATH_PPPOE);p.pppoe_sid=0;CHECK(!hnat_tunnel_outer_path(&s,&ppp,&p,&t));p.pppoe_sid=123;
 physical.running=false;CHECK(!hnat_tunnel_outer_path(&s,&ppp,&p,&t));physical.running=true;
}
static void neighbours(void){
 struct dst_entry dst={.dev=&ppp};struct sk_buff skb={.dst=&dst,.protocol=htons(ETH_P_IP)};struct flow_offload_hw_path p={.flags=BIT(DEV_PATH_PPPOE),.pppoe_sid=123};
 memset(p.eth_src,0x20,6);memset(p.eth_dest,0x40,6);CHECK(hnat_tunnel_outer_neighbour(&skb,&ppp,&p));CHECK(arp_reads==0);
 p.pppoe_sid=0;CHECK(!hnat_tunnel_outer_neighbour(&skb,&ppp,&p));p.pppoe_sid=123;p.eth_dest[0]=1;CHECK(!hnat_tunnel_outer_neighbour(&skb,&ppp,&p));
 p.flags=BIT(DEV_PATH_VLAN);dst.dev=&vlan;CHECK(hnat_tunnel_outer_neighbour(&skb,&vlan,&p));CHECK(arp_reads==1&&releases==1);CHECK(!memcmp(p.eth_dest,neighbour.ha,6));
 neighbour.nud_state=0;CHECK(!hnat_tunnel_outer_neighbour(&skb,&vlan,&p));CHECK(releases==2);neighbour.nud_state=1;
}
static void native_vlan(bool baseline){
 struct vlan_dev_priv vp={.real_dev=&physical,.vlan_proto=htons(ETH_P_8021Q),.vlan_id=35};vlan.priv=&vp;
 struct flow_offload_hw_path path={.dev=&vlan,.flags=BIT(DEV_PATH_ETHERNET)};
 CHECK((baseline?baseline_vlan_check(&path):vlan_dev_flow_offload_check(&path))==0);
 CHECK(path.vlan_id==35&&path.dev==&physical&&(path.flags&BIT(DEV_PATH_VLAN)));
 path.dev=&vlan;CHECK((baseline?baseline_vlan_check(&path):vlan_dev_flow_offload_check(&path))==-EEXIST);
}
static void ack(struct npu_tnl_info *i){i->tnl_params=i->cache;i->status=TNL_STA_UPDATED;}
static void delete_for_device(struct npu_tnl_info *i){CHECK(i->lock.held);live_binding=false;i->status=TNL_STA_DELETING;}
static bool use_irqsave_baseline;
static int submit_offload(struct npu_tnl_input *in,struct npu_tnl_type *type,struct npu_tnl_info *info,struct npu_tnl_params *params){
 unsigned bh=test_bh_depth,irq=test_irq_depth;CHECK(test_info==info);
 int ret=(use_irqsave_baseline?baseline_lock_offload:mtk_npu_tnl_offload)(in,type,params,mtk_npu_nf_hnat_tnl_info_setup);
 CHECK(test_bh_depth==bh&&test_irq_depth==irq);CHECK(!npu_tnl.tbl_lock.held&&!info->lock.held);CHECK(test_bad_bh_enable==0);return ret;
}
static void outer_lock_errors(void){
 struct npu_tnl_info info={0};struct npu_tnl_type type={0};struct npu_tnl_params params={0};struct npu_tnl_input input={0};test_info=&info;
 CHECK(mtk_npu_tnl_offload(NULL,NULL,NULL,NULL)==-EPERM);
 find_error=-EIO;CHECK(mtk_npu_tnl_offload(&input,&type,&params,mtk_npu_nf_hnat_tnl_info_setup)==-EIO);
 find_error=-ENODEV;alloc_error=-ENOMEM;CHECK(mtk_npu_tnl_offload(&input,&type,&params,mtk_npu_nf_hnat_tnl_info_setup)==-ENOMEM);
 find_error=alloc_error=0;CHECK(mtk_npu_tnl_offload(&input,&type,&params,mtk_npu_nf_hnat_tnl_info_setup)==-EPERM);
 CHECK(!test_bh_depth&&!test_irq_depth&&!npu_tnl.tbl_lock.held&&!info.lock.held);
}
static int retry_before_queue(struct npu_tnl_input *in,struct npu_tnl_type *type,struct npu_tnl_info *info,struct npu_tnl_params *params){(void)in;(void)type;(void)info;(void)params;return -EAGAIN;}
static void allocation_failures(bool baseline){
 struct npu_tnl_type type={.max_mtu=1500};struct npu_tnl_params params={0};struct npu_tnl_input bad={0};
 int (*offload)(struct npu_tnl_input *,struct npu_tnl_type *,struct npu_tnl_params *,tnl_info_setup_func_t)=baseline?baseline_alloc_offload:mtk_npu_tnl_offload;
 memset(npu_tnl.tnl_infos,0,sizeof(npu_tnl.tnl_infos));npu_tnl.tnl_used[0]=0;
 for(unsigned i=0;i<32;i++)npu_tnl.tnl_infos[i].tnl_idx=i;
 use_native_allocation=true;find_error=-ENODEV;
 for(unsigned i=0;i<64;i++){
  CHECK(offload(&bad,&type,&params,mtk_npu_nf_hnat_tnl_info_setup)==-EPERM);
  CHECK(npu_tnl.tnl_used[0]==0);CHECK(npu_tnl.tnl_infos[0].status==TNL_STA_UNINIT);
 }
 CHECK(offload(&bad,&type,&params,retry_before_queue)==-EAGAIN);CHECK(!npu_tnl.tnl_used[0]);
 struct sk_buff skb={.encap=true};struct hnat_tunnel_token token=path_case(BIT(DEV_PATH_PPPOE)|BIT(DEV_PATH_VLAN));struct npu_tnl_input good={.hnat={.skb=&skb,.dev=&tunnel,.generation=&token.generation,.token=&token}};
 params.params.network.type=NPU_NETWORK_IP;params.params.network.ip.ttl=64;
 CHECK(offload(&good,&type,&params,mtk_npu_nf_hnat_tnl_info_setup)==-EAGAIN);
 struct npu_tnl_info *info=&npu_tnl.tnl_infos[0];CHECK(npu_tnl.tnl_used[0]==1&&(info->status&TNL_STA_QUEUED));CHECK(physical.refs==1&&ppp.refs==1&&tunnel.refs==1);
 find_error=0;test_info=info;CHECK(offload(&bad,&type,&params,mtk_npu_nf_hnat_tnl_info_setup)==-EPERM);CHECK(npu_tnl.tnl_used[0]==1&&(info->status&TNL_STA_QUEUED));
 spin_lock_bh(&npu_tnl.tbl_lock);spin_lock(&info->lock);mtk_npu_tnl_info_free_no_lock(info);spin_unlock(&info->lock);spin_unlock_bh(&npu_tnl.tbl_lock);
 CHECK(!npu_tnl.tnl_used[0]&&!physical.refs&&!ppp.refs&&!tunnel.refs);use_native_allocation=false;find_error=0;
}
static void lease_tests(void){
 struct npu_tnl_info info={0};struct npu_tnl_type type={.max_mtu=1500};struct npu_tnl_params params={0};struct sk_buff skb={.encap=true};
 struct hnat_tunnel_token token=path_case(BIT(DEV_PATH_PPPOE)|BIT(DEV_PATH_VLAN));struct npu_tnl_input input={.hnat={.skb=&skb,.dev=&tunnel,.generation=&token.generation,.token=&token}};
 test_info=&info;token.index=0;params.params.network.type=NPU_NETWORK_IP;params.params.network.ip.ttl=64;
 CHECK(submit_offload(&input,&type,&info,&params)==-EAGAIN);CHECK(flushes==0);CHECK(info.dev==&tunnel&&physical.refs==1&&ppp.refs==1&&tunnel.refs==1);
 CHECK(!mtk_npu_bind_lock(&tunnel,&token));ack(&info);
 CHECK(submit_offload(&input,&type,&info,&params)==0);CHECK(mtk_npu_bind_lock(&tunnel,&token));mtk_npu_bind_unlock(&token);
 u64 old_gen=token.generation;live_binding=true;token.pppoe_sid++;
 CHECK(submit_offload(&input,&type,&info,&params)==-EAGAIN);CHECK(flushes==1&&!live_binding&&info.generation!=old_gen);
 CHECK(!mtk_npu_bind_lock(&tunnel,&token));ack(&info);CHECK(submit_offload(&input,&type,&info,&params)==0);CHECK(mtk_npu_bind_lock(&tunnel,&token));mtk_npu_bind_unlock(&token);
 live_binding=true;token.vlan_tci^=1;CHECK(submit_offload(&input,&type,&info,&params)==-EAGAIN);CHECK(flushes==2&&!live_binding);ack(&info);CHECK(submit_offload(&input,&type,&info,&params)==0);
 token.pppoe_sid++;CHECK(!mtk_npu_bind_lock(&tunnel,&token));token.pppoe_sid--;ppp.running=false;CHECK(!mtk_npu_bind_lock(&tunnel,&token));ppp.running=true;
 CHECK(physical.refs==1&&ppp.refs==1&&tunnel.refs==1);npu_tnl.ht[0]=&info;live_binding=true;
 mtk_npu_tnl_info_match_netdev_and_down(&ppp,delete_for_device);CHECK(!live_binding&&info.status==TNL_STA_DELETING);CHECK(!mtk_npu_bind_lock(&tunnel,&token));
 CHECK(physical.refs==0&&ppp.refs==0&&tunnel.refs==0);mtk_npu_tnl_info_put_devices(&info);CHECK(physical.refs==0);npu_tnl.ht[0]=NULL;
 memset(&info,0,sizeof(info));info.dev=&tunnel;dev_hold(&tunnel);token=path_case(0);mtk_npu_egress_replace(&info,&token);CHECK(physical.refs==2);mtk_npu_tnl_info_put_devices(&info);CHECK(physical.refs==0&&tunnel.refs==0);
}
static void firmware_bounds(void){
 struct platform_device p={0};npu.phys_base=0x09100000;npu.size=0x1000000;mcu_count=1;mcu_error=0;
 mcu_address=npu.phys_base;CHECK(!mtk_npu_tnl_offload_post_init(&p));CHECK(npu_tnl.tbl_addr==0);
 mcu_address=npu.phys_base+npu.size-sizeof(struct npu_tnl_params)*32;CHECK(!mtk_npu_tnl_offload_post_init(&p));
 mcu_address+=4;CHECK(mtk_npu_tnl_offload_post_init(&p)==-ERANGE);
 mcu_address=npu.phys_base-4;CHECK(mtk_npu_tnl_offload_post_init(&p)==-ERANGE);
 mcu_address=npu.phys_base+1;CHECK(mtk_npu_tnl_offload_post_init(&p)==-ERANGE);
 mcu_address=npu.phys_base;mcu_count=2;CHECK(mtk_npu_tnl_offload_post_init(&p)==-EIO);
 mcu_count=1;mcu_error=-ETIMEDOUT;CHECK(mtk_npu_tnl_offload_post_init(&p)==-ETIMEDOUT);
}
int main(int argc,char **argv){init_devices();if(argc>1&&!strcmp(argv[1],"baseline")){native_vlan(true);return 0;}
 if(argc>1&&!strcmp(argv[1],"baseline-lock")){use_irqsave_baseline=true;lease_tests();return 0;}
 if(argc>1&&!strcmp(argv[1],"baseline-allocation")){allocation_failures(true);return 0;}
 if(argc>1&&!strcmp(argv[1],"required-ipv6")){struct npu_params a={0},b={0};a.network.type=b.network.type=NPU_NETWORK_IPV6;CHECK(mtk_npu_params_match(&a,&b));return 0;}
 for(unsigned i=0;i<4;i++)path_case((i&1?BIT(DEV_PATH_PPPOE):0)|(i&2?BIT(DEV_PATH_VLAN):0));
 path_rejections();neighbours();native_vlan(false);outer_lock_errors();lease_tests();
 CHECK(!test_bh_depth&&!test_irq_depth);flushes=submits=0;init_devices();test_bh_depth=1;lease_tests();CHECK(test_bh_depth==1&&!test_irq_depth);test_bh_depth=0;
 allocation_failures(false);firmware_bounds();printf("PASS %u assertions: native VLAN/path/PPE, actual offload/allocate/free/lease callbacks, BH/IRQ context/exit pairing, session retirement, device refs and firmware bounds; services shimmed\n",checks);return 0;}
