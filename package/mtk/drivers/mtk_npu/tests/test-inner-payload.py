#!/usr/bin/env python3
"""Run actual tunnel payload/identity functions with skb reads modeled."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[5]
path = root / 'target/linux/mediatek/files-6.6/drivers/net/ethernet/mediatek/mtk_hnat/hnat_nf_hook.c'
text = path.read_text()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)

def function(source, name):
    start = re.search(r'(?m)^static bool ' + name + r'\(', source).start()
    brace = source.index('{', start)
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda m: ' ' * len(m[0]), source, flags=re.S)
    depth = 0
    for at in range(brace, len(source)):
        depth += (masked[at] == '{') - (masked[at] == '}')
        if not depth:
            return source[start:at + 1] + '\n'
    raise ValueError(name)

functions = [function(text, n) for n in
             ('hnat_tunnel_ipv4_payload_valid', 'hnat_tunnel_inner_match')]
baseline = (Path(__file__).resolve().parent/'fixtures/inner-tcp-baseline.c').read_text()
baseline = function(baseline, 'hnat_tunnel_inner_match').replace('hnat_tunnel_inner_match(', 'baseline_match(', 1)
shim = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
typedef uint32_t u32;
#define IPV4_HNAPT 0
struct tcpudphdr { uint16_t src,dst; };
struct foe_entry {
 struct { unsigned pkt_type,udp; } bfib1;
 struct { uint32_t new_sip,new_dip;uint16_t new_sport,new_dport; } ipv4_hnapt;
};
struct sk_buff { unsigned char *data;unsigned len;int inner;
 bool encapsulation,gso,nonlinear;int fail_offset; };
#define skb_is_gso(s) ((s)->gso)
#define skb_inner_network_header(s) ((s)->data+(s)->inner)
static bool ip_is_fragment(const struct iphdr *ip) { return !!(ntohs(ip->frag_off)&0x3fff); }
static const void *skb_header_pointer(const struct sk_buff *s,int off,int len,void *copy) {
 if(off<0 || (unsigned)off>s->len || (unsigned)len>s->len-(unsigned)off || off==s->fail_offset)return NULL;
 if(s->nonlinear){memcpy(copy,s->data+off,len);return copy;}
 return s->data+off;
}
'''
cases = r'''
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static unsigned char bytes[256] __attribute__((aligned(16)));
static struct sk_buff skb;
static struct foe_entry entry;
static void init(bool udp) {
 memset(bytes,0,sizeof(bytes));memset(&skb,0,sizeof(skb));memset(&entry,0,sizeof(entry));
 skb.data=bytes;skb.len=128;skb.inner=64;skb.encapsulation=true;skb.fail_offset=-1;
 struct iphdr *ip=(void*)(bytes+64);ip->version=4;ip->ihl=5;
 ip->protocol=udp?IPPROTO_UDP:IPPROTO_TCP;ip->tot_len=htons(udp?28:40);
 ip->saddr=htonl(0xc0000201);ip->daddr=htonl(0xc6336401);
 struct tcpudphdr *p=(void*)(bytes+84);p->src=htons(12345);p->dst=htons(443);
 if(udp)((struct udphdr *)p)->len=htons(8);else((struct tcphdr *)p)->doff=5;
 entry.bfib1.pkt_type=IPV4_HNAPT;entry.bfib1.udp=udp;
 entry.ipv4_hnapt.new_sip=0xc0000201;entry.ipv4_hnapt.new_dip=0xc6336401;
 entry.ipv4_hnapt.new_sport=12345;entry.ipv4_hnapt.new_dport=443;
}
int main(void) {
 for(unsigned nonlinear=0;nonlinear<2;nonlinear++)for(unsigned udp=0;udp<2;udp++) {
  init(udp);skb.nonlinear=nonlinear;CHECK(hnat_tunnel_inner_match(&skb,&entry));
  CHECK(udp ? !baseline_match(&skb,&entry) : baseline_match(&skb,&entry));
  struct iphdr *ip=(void*)(bytes+64);
  entry.bfib1.udp=!udp;CHECK(!hnat_tunnel_inner_match(&skb,&entry));entry.bfib1.udp=udp;
  entry.ipv4_hnapt.new_sport++;CHECK(!hnat_tunnel_inner_match(&skb,&entry));entry.ipv4_hnapt.new_sport--;
  entry.ipv4_hnapt.new_dip++;CHECK(!hnat_tunnel_inner_match(&skb,&entry));entry.ipv4_hnapt.new_dip--;
  skb.gso=true;CHECK(!hnat_tunnel_inner_match(&skb,&entry));skb.gso=false;
  skb.encapsulation=false;CHECK(!hnat_tunnel_inner_match(&skb,&entry));skb.encapsulation=true;
  ip->frag_off=htons(0x2000);CHECK(!hnat_tunnel_inner_match(&skb,&entry));ip->frag_off=0;
  ip->ihl=6;CHECK(!hnat_tunnel_inner_match(&skb,&entry));ip->ihl=5;
  ip->protocol=IPPROTO_ICMP;CHECK(!hnat_tunnel_inner_match(&skb,&entry));ip->protocol=udp?IPPROTO_UDP:IPPROTO_TCP;
  skb.fail_offset=84;CHECK(!hnat_tunnel_inner_match(&skb,&entry));skb.fail_offset=-1;
  skb.len=udp?91:103;CHECK(!hnat_tunnel_inner_match(&skb,&entry));skb.len=128;
  if(udp) {
   struct udphdr *u=(void*)(bytes+84);
   u->len=htons(7);CHECK(!hnat_tunnel_inner_match(&skb,&entry));
   u->len=htons(9);CHECK(!hnat_tunnel_inner_match(&skb,&entry));
   ip->tot_len=htons(29);CHECK(hnat_tunnel_inner_match(&skb,&entry));
   u->check=htons(0x1234);CHECK(hnat_tunnel_inner_match(&skb,&entry));
   ip->tot_len=htons(27);CHECK(!hnat_tunnel_inner_match(&skb,&entry));
  } else {
   struct tcphdr *t=(void*)(bytes+84);
   t->doff=4;CHECK(!hnat_tunnel_inner_match(&skb,&entry));
   t->doff=6;CHECK(!hnat_tunnel_inner_match(&skb,&entry));
   ip->tot_len=htons(44);CHECK(hnat_tunnel_inner_match(&skb,&entry));
  }
 }
 printf("PASS %u assertions: actual IPv4 TCP/UDP payload and inner identity functions; skb reads modeled\n",checks);
 return 0;
}
'''
program = shim + '\n'.join(functions) + baseline + cases
c = out / 'actual-inner-payload.c'
c.write_text(program)
commands = []
for argv in [['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
              '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(c),
              '-o', str(out/'actual-inner-payload')], [str(out/'actual-inner-payload')]]:
    result = subprocess.run(argv, capture_output=True, text=True)
    commands.append({'argv': argv, 'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr})
    if result.returncode:
        break
receipt = {'scope': __doc__, 'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
           'baseline': '1ea2dd3a50e166445d8997211840f519285dd114', 'commands': commands,
           'hardware_forwarding_proof': False}
(out/'receipt.json').write_text(json.dumps(receipt, indent=2)+'\n')
print(json.dumps(receipt))
raise SystemExit(commands[-1]['exit'])
