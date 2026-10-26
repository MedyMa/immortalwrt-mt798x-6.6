#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <linux/if_ether.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;typedef uint16_t __be16;typedef uint32_t __be32;
#define __packed __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#define CONFIG_MEDIATEK_NETSYS_V3 1
#define CONFIG_NPU_TNL_TYPE_NUM 32
#define BIT(n) (1U<<(n))
#define VLAN_VID_MASK 4095
#define READ_ONCE(x) (x)
#define unlikely(x) (x)
#define IS_ERR(p) ((uintptr_t)(p)>(uintptr_t)-4096)
struct list_head { void *next,*prev; };struct hlist_node {void *next,*pprev;};
typedef struct {bool held;} spinlock_t;
static unsigned test_bh_depth,test_irq_depth,test_bad_bh_enable;
static void spin_lock(spinlock_t *s){assert(!s->held);s->held=true;}
static void spin_unlock(spinlock_t *s){assert(s->held);s->held=false;}
static void spin_lock_bh(spinlock_t *s){++test_bh_depth;spin_lock(s);}
static void spin_unlock_bh(spinlock_t *s){spin_unlock(s);assert(test_bh_depth);if(test_irq_depth)++test_bad_bh_enable;--test_bh_depth;}
#define spin_lock_irqsave(s,f) do{(f)=test_irq_depth;++test_irq_depth;spin_lock(s);}while(0)
#define spin_unlock_irqrestore(s,f) do{spin_unlock(s);test_irq_depth=(f);}while(0)
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define ERR_PTR(e) ((void *)(intptr_t)(e))
struct net {int unused;};static struct net init_net;
struct net_device_ops;struct net_device {
 bool running,physical,dsa,vlan;int refs;unsigned mtu;unsigned char dev_addr[6];struct net *net;
 struct net_device *real;u16 vid,qos;void *priv;const struct net_device_ops *netdev_ops;
};
static bool netif_running(const struct net_device *d){return d->running;}
static bool netdev_uses_dsa(const struct net_device *d){return d->dsa;}
static struct net *dev_net(const struct net_device *d){return d->net;}
static bool is_vlan_dev(const struct net_device *d){return d->vlan;}
static struct net_device *vlan_dev_real_dev(const struct net_device *d){return d->real;}
static u16 vlan_dev_vlan_id(const struct net_device *d){return d->vid;}
static u16 vlan_dev_get_egress_qos_mask(const struct net_device *d,u32 priority){(void)priority;return d->qos;}
static void dev_hold(struct net_device *d){assert(d);++d->refs;}
static void dev_put(struct net_device *d){assert(d&&d->refs>0);--d->refs;}
static void *netdev_priv(struct net_device *d){return d->priv;}
struct mtk_mac{int id;};static struct mtk_mac mac;
static struct mtk_mac *hnat_tunnel_eth_mac(const struct net_device *d){return d->physical?&mac:NULL;}
struct dst_entry{struct net_device *dev;bool xfrm;};
struct sk_buff{u32 priority;u16 protocol;struct dst_entry *dst;bool encap,decap;unsigned tops;struct net_device *dev;};
static struct dst_entry *skb_dst(struct sk_buff *s){return s->dst;}
static bool dst_xfrm(struct dst_entry *d){return d->xfrm;}
struct neighbour{unsigned char ha[6];u32 nud_state;};static struct neighbour neighbour;
static unsigned arp_reads,releases;static bool have_neighbour=true;
#define NUD_VALID 1
static struct neighbour *dst_neigh_lookup_skb(struct dst_entry *d,struct sk_buff *s){(void)d;(void)s;++arp_reads;return have_neighbour?&neighbour:NULL;}
static void neigh_ha_snapshot(u8 *dest,struct neighbour *n,const struct net_device *d){(void)d;memcpy(dest,n->ha,6);}
static void neigh_release(struct neighbour *n){(void)n;++releases;}
static bool is_valid_ether_addr(const u8 *p){return !(p[0]&1)&&memcmp(p,"\0\0\0\0\0\0",6);}
static void ether_addr_copy(u8 *d,const u8 *s){memcpy(d,s,6);}
struct flow_offload_hw_path;
struct net_device_ops {int(*ndo_flow_offload_check)(struct flow_offload_hw_path *);};
struct vlan_dev_priv{struct net_device *real_dev;u16 vlan_proto,vlan_id;};
static struct vlan_dev_priv *vlan_dev_priv(struct net_device *d){return netdev_priv(d);}
