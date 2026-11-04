#!/usr/bin/env python3
"""Exercise NPU mailbox/statistics and FLOW failure paths on the host.

Production functions are extracted unchanged; transport, locks and sysfs are
modeled. This does not execute firmware, DMA or hardware forwarding.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--source-tree', type=Path, default=Path(__file__).resolve().parents[2])
a = p.parse_args()
root = a.source_tree.resolve()
bases = [root/'package/mtk/drivers/mtk_npu/src', root/'package/kernel/mtk_npu/src']
base = next(path for path in bases if path.is_dir())

def function(source, name):
    m = re.search(r'(?m)^(?:static )?(?:inline )?(?:int|ssize_t|void|bool)\s+'+name+r'\(', source)
    if not m:
        raise ValueError('missing production function: '+name)
    start = source.index('{', m.start()); end = start+1; depth = 1
    while depth:
        depth += (source[end] == '{')-(source[end] == '}'); end += 1
    return source[m.start():end]

mac = function((base/'npu-dpdk/mac-filter.c').read_text(), 'mtk_npu_dpdk_mac_filter_drop_pkt_count')
show = function((base/'npu-dpdk/sysfs.c').read_text(), 'dpdk_mac_filter_drop_pkt_show')
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
typedef uint32_t u32;
#define PAGE_SIZE 4096
#define scnprintf snprintf
#define NPU_NET_CMD_TYPE_DPDK 1
#define NPU_DPDK_NET_CMD_PARAMS 2
#define NPU_DPDK_NET_CMD_MAC_FILTER_PARAMS_DROP_PKT_GET 3
#define NET_RET_CNT_1 1
struct device {int unused;}; struct device_attribute {int unused;};
struct net_cmd {int type,sub_type;u32 arg[4],ret[4];int return_cnt;};
static int command_error,return_count=1,calls,access_error,begins,ends,active;
static u32 response=UINT32_MAX;
static int mtk_npu_net_send_cmd_mgmt(struct net_cmd *cmd){
 assert(cmd->type==NPU_NET_CMD_TYPE_DPDK&&cmd->sub_type==NPU_DPDK_NET_CMD_PARAMS);
 assert(cmd->arg[0]==NPU_DPDK_NET_CMD_MAC_FILTER_PARAMS_DROP_PKT_GET);calls++;
 if(command_error){return command_error;}cmd->return_cnt=return_count;cmd->ret[0]=response;return 0;
}
static int mtk_npu_dpdk_access_begin(void){begins++;if(access_error)return access_error;active++;return 0;}
static void mtk_npu_dpdk_access_end(void){assert(active==1);active--;ends++;}
'''
program += '\n'+mac+'\n'+show
program += r'''
int main(void){u32 count=19;char buf[PAGE_SIZE];
 assert(mtk_npu_dpdk_mac_filter_drop_pkt_count(NULL)==-EINVAL&&calls==0);
 command_error=-ETIMEDOUT;assert(mtk_npu_dpdk_mac_filter_drop_pkt_count(&count)==-ETIMEDOUT&&count==19);
 command_error=0;return_count=0;assert(mtk_npu_dpdk_mac_filter_drop_pkt_count(&count)==-EIO&&count==19);
 return_count=2;assert(mtk_npu_dpdk_mac_filter_drop_pkt_count(&count)==-EIO&&count==19);
 return_count=1;assert(!mtk_npu_dpdk_mac_filter_drop_pkt_count(&count)&&count==UINT32_MAX);
 response=0;assert(!mtk_npu_dpdk_mac_filter_drop_pkt_count(&count)&&count==0);
 response=123;assert(dpdk_mac_filter_drop_pkt_show(NULL,NULL,buf)>0);
 command_error=-ETIMEDOUT;assert(dpdk_mac_filter_drop_pkt_show(NULL,NULL,buf)==-ETIMEDOUT);
 return_count=0;command_error=0;assert(dpdk_mac_filter_drop_pkt_show(NULL,NULL,buf)==-EIO);
'''
if 'mtk_npu_dpdk_access_begin()' in show:
    program += r'''
 assert(begins==3&&ends==3&&active==0);
 access_error=-ENODEV;int before=calls;
 assert(dpdk_mac_filter_drop_pkt_show(NULL,NULL,buf)==-ENODEV&&calls==before&&active==0&&ends==3);
'''
program += '\nreturn 0;}\n'
programs = [('mac-filter-errors', program)]
flow = (base/'npu-flow/tnl-offload.c').read_text()
pptp = (base/'protocol/tunnel/pptp/pptp.c').read_text()
if 'static int pptp_tnl_flow_param_setup' in pptp:
    setup = function(pptp, 'pptp_tnl_flow_param_setup')
    caller = function(flow, 'mtk_npu_flow_tnl_offload')
    assert re.search(r'ret\s*=\s*tnl_type->tnl_flow_param_setup\([^;]+;\s*if\s*\(ret\)\s*return ret;', caller)
    allocation = function((base/'protocol/tunnel/pptp/pptp_seq.c').read_text(), 'mtk_npu_pptp_seq_alloc_no_lock')
    assert 'GFP_ATOMIC' in allocation and 'GFP_KERNEL' not in allocation
    prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
typedef uint8_t u8;
#define ETH_ALEN 6
#define ETH_P_IP 0x0800
#define NPU_MAC_ETH 1
#define NPU_NETWORK_IP 1
#define NPU_TUNNEL_PPTP 1
struct npu_pptp_params {uint16_t dl_call_id,ul_call_id;u8 seq_gen_idx;};
struct npu_ip_params {uint32_t sip,dip;u8 proto;};
struct npu_params {struct {int type;struct {u8 h_dest[6],h_source[6];uint16_t h_proto;} eth;} mac;struct {int type;struct npu_ip_params ip;} network;struct {int type;struct npu_pptp_params pptp;} tunnel;};
struct net_device_path {struct {u8 h_dest[6],h_source[6];struct {uint32_t ip;} sip,dip;struct {uint16_t call_id,peer_call_id;uint32_t seq_sent;} pptp;} tunnel;};
static int sequence_error,index_value;
static int pptp_get_seq_gen_idx(uint16_t call,uint32_t seq,int *index){assert(call==123&&seq==44);if(sequence_error){return sequence_error;}*index=index_value;return 0;}
'''
    suffix = r'''
int main(void){struct net_device_path path={.tunnel={.pptp={.call_id=321,.peer_call_id=123,.seq_sent=44},.sip={.ip=1},.dip={.ip=2}}};struct npu_params params={};
 sequence_error=-ENOMEM;assert(pptp_tnl_flow_param_setup(&path,&params)==-ENOMEM);
 sequence_error=-EBUSY;assert(pptp_tnl_flow_param_setup(&path,&params)==-EBUSY);
 sequence_error=0;index_value=0;assert(!pptp_tnl_flow_param_setup(&path,&params)&&params.tunnel.pptp.seq_gen_idx==0);
 index_value=31;assert(!pptp_tnl_flow_param_setup(&path,&params)&&params.tunnel.pptp.seq_gen_idx==31);
 assert(params.network.ip.sip==1&&params.network.ip.dip==2&&params.network.ip.proto==IPPROTO_GRE);
 assert(params.tunnel.pptp.ul_call_id==htons(123)&&params.tunnel.pptp.dl_call_id==htons(321));
 return 0;}
'''
    programs.append(('pptp-flow-failure', prefix+setup+suffix))

with tempfile.TemporaryDirectory(prefix='npu-sync-') as tmp:
    for name, code in programs:
        src = Path(tmp)/(name+'.c'); binary = Path(tmp)/name
        src.write_text(code)
        subprocess.run(['cc','-std=gnu11','-Wall','-Werror','-Wno-unused-function',
                        '-fsanitize=address,undefined','-fno-omit-frame-pointer',str(src),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
        print('PASS '+name)
print(f'programs={len(programs)}; hardware_test=false')
