#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define __packed __attribute__((packed))
#define __iomem
#define CONFIG_MEDIATEK_NETSYS_V3 1
#define CONFIG_NF_HNAT_PACKET_ACCOUNTING 1
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define GFP_ATOMIC 0
#define BIT(x) (1U<<(x))
#define GENMASK(h,l) ((~0U<<(l)) & (~0U>>(31-(h))))
#define MAX_PPE_NUM 3
#define CFG_PPE_NUM (hnat_priv->ppe_num)
#define IP_CT_DIR_MAX 2
#define CTINFO2DIR(i) ((i)>=IP_CT_IS_REPLY)
#define container_of(ptr,type,member) ((type *)((char *)(ptr)-offsetof(type,member)))
#define IS_IPV4_HNAPT(e) ((e)->bfib1.pkt_type==IPV4_HNAPT)
#define IS_IPV4_HNAT(e) ((e)->bfib1.pkt_type==IPV4_HNAT)
#define IS_IPV6_5T_ROUTE(e) ((e)->bfib1.pkt_type==IPV6_5T_ROUTE)
#define IS_IPV6_HNAPT(e) ((e)->bfib1.pkt_type==IPV6_HNAPT)
#define IS_IPV6_HNAT(e) ((e)->bfib1.pkt_type==IPV6_HNAT)
enum ip_conntrack_info { IP_CT_ESTABLISHED=0, IP_CT_NEW=2, IP_CT_IS_REPLY=3 };
enum { MTK_HNAT_V3=5 };
typedef struct { bool held; } spinlock_t;
static void spin_lock_bh(spinlock_t *s) { assert(!s->held);s->held=true; }
static void spin_unlock_bh(spinlock_t *s) { assert(s->held);s->held=false; }
#define lockdep_assert_held(s) assert((s)->held)
typedef struct { u64 value; } atomic64_t;
static void atomic64_add(u64 n,atomic64_t *s) { s->value+=n; }
struct delayed_work { int unused; };
struct rcu_head { struct rcu_head *next; void (*fn)(struct rcu_head *); };
struct nf_conntrack_zone { u16 id,dir,flags; };
struct hnat_account_net { bool exiting; };
struct net { struct hnat_account_net state; };
struct nf_conn_acct { struct { atomic64_t bytes,packets; } counter[2]; };
struct nf_conn { int ct_general; struct net net; struct nf_conntrack_zone zone;
 bool dying,templ,no_acct; struct nf_conn_acct acct; };
struct sk_buff { struct nf_conn *ct; enum ip_conntrack_info info;
 bool prepared,published,finished;u64 bytes,packets; };
static unsigned int account_net_id;
static unsigned long jiffies=1000;
static bool fail_alloc;
static struct rcu_head *retired_rcu;
static void *kzalloc(size_t n,int flags) { assert(flags==GFP_ATOMIC);return fail_alloc?NULL:calloc(1,n); }
static void kfree(void *p) { free(p); }
static void nf_conntrack_get(int *r) { ++*r; }
static void nf_ct_put(struct nf_conn *c) { assert(c->ct_general>0);--c->ct_general; }
static void call_rcu(struct rcu_head *r,void (*fn)(struct rcu_head *)) { r->next=retired_rcu;r->fn=fn;retired_rcu=r; }
static void reap_rcu(void) { while(retired_rcu) { struct rcu_head *r=retired_rcu;retired_rcu=r->next;r->fn(r); } }
static struct nf_conn *nf_ct_get(struct sk_buff *s,enum ip_conntrack_info *i) { *i=s->info;return s->ct; }
static struct net *nf_ct_net(struct nf_conn *c) { return &c->net; }
static struct nf_conntrack_zone *nf_ct_zone(struct nf_conn *c) { return &c->zone; }
static bool nf_ct_is_dying(struct nf_conn *c) { return c->dying; }
static bool nf_ct_is_template(struct nf_conn *c) { return c->templ; }
static struct nf_conn_acct *nf_conn_acct_find(struct nf_conn *c) { return c->no_acct?NULL:&c->acct; }
static void *net_generic(struct net *n,unsigned int id) { (void)id;return &n->state; }
static bool nf_hnat_packet_accounting_prepare(struct sk_buff *s) { if(s->finished||s->published)return false;s->prepared=true;return true; }
static bool nf_hnat_packet_accounting_publish(struct sk_buff *s,u64 bytes,u64 packets) { if(!s->prepared||s->published)return false;s->bytes=bytes;s->packets=packets;s->published=true;return true; }
static void nf_hnat_packet_accounting_finish_zero(struct sk_buff *s) { if(s)s->finished=true; }
#define wmb() ((void)0)
#define dma_wmb() ((void)0)
