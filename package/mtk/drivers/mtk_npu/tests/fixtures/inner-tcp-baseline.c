/* SPDX-License-Identifier: GPL-2.0 */
/* Exact function from 1ea2dd3a50e166445d8997211840f519285dd114; services supplied by the host harness. */
static bool hnat_tunnel_inner_match(const struct sk_buff *skb,
				  const struct foe_entry *entry)
{
	struct iphdr ip_copy;
	struct tcpudphdr ports_copy;
	const struct iphdr *ip;
	const struct tcpudphdr *ports;
	int offset;

	if (!skb->encapsulation || skb_is_gso(skb) ||
	    entry->bfib1.pkt_type != IPV4_HNAPT)
		return false;
	offset = skb_inner_network_header(skb) - skb->data;
	if (offset < 0 || offset > skb->len || skb->len - offset < sizeof(ip_copy))
		return false;
	ip = skb_header_pointer(skb, offset, sizeof(ip_copy), &ip_copy);
	if (!ip || ip->version != 4 || ip->ihl < 5 || ip_is_fragment(ip) ||
	    ip->protocol != IPPROTO_TCP || ntohs(ip->tot_len) < ip->ihl * 4 + sizeof(struct tcphdr) ||
	    ntohs(ip->tot_len) > skb->len - offset)
		return false;
	ports = skb_header_pointer(skb, offset + ip->ihl * 4,
				   sizeof(ports_copy), &ports_copy);
	return ports && !entry->bfib1.udp &&
	       ntohl(ip->saddr) == entry->ipv4_hnapt.new_sip &&
	       ntohl(ip->daddr) == entry->ipv4_hnapt.new_dip &&
	       ntohs(ports->src) == entry->ipv4_hnapt.new_sport &&
	       ntohs(ports->dst) == entry->ipv4_hnapt.new_dport;
}
