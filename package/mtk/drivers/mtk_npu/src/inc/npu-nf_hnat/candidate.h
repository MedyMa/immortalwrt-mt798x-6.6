/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NPU_NF_HNAT_CANDIDATE_H_
#define _NPU_NF_HNAT_CANDIDATE_H_

#include <linux/ip.h>
#include <linux/in.h>
#include <linux/skbuff.h>

/* A read-only prefilter for the IPv4 GRE/VXLAN/L2TP backends below. This is
 * only a candidate test: the registered protocol still validates its complete
 * header, session and policy before changing HNAT metadata. In particular this
 * must not pull, linearize or write a header shared with another skb.
 */
static inline bool mtk_npu_tunnel_candidate(const struct sk_buff *skb)
{
	struct iphdr ip_copy;
	const struct iphdr *ip;
	struct { __be16 source, dest; } ports_copy;
	const typeof(ports_copy) *ports;
	int offset = skb_network_offset(skb);

	if (offset < 0)
		return false;
	ip = skb_header_pointer(skb, offset, sizeof(ip_copy), &ip_copy);
	if (!ip)
		return false;
	if (ip != &ip_copy)
		memcpy(&ip_copy, ip, sizeof(ip_copy));
	ip = &ip_copy;
	if (ip->version != 4 || ip->ihl != 5 || ip_is_fragment(ip))
		return false;
	if (ip->protocol == IPPROTO_GRE)
		return true;
	if (ip->protocol != IPPROTO_UDP)
		return false;
	ports = skb_header_pointer(skb, offset + sizeof(ip_copy),
				   sizeof(ports_copy), &ports_copy);
	if (!ports)
		return false;
	if (ports != &ports_copy)
		memcpy(&ports_copy, ports, sizeof(ports_copy));
	ports = &ports_copy;
	return ports->dest == htons(4789) ||
		(ports->source == htons(1701) && ports->dest == htons(1701));
}

#endif
