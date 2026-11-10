// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 MediaTek Inc.
 *
 * Author: Chris.Chou <chris.chou@mediatek.com>
 *         Ren-Ting Wang <ren-ting.wang@mediatek.com>
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/rtnetlink.h>
#include <mtk_eth_soc.h>
#include <mtk_eth_reset.h>
#include <mtk_hnat/hnat.h>
#include <pce/netsys.h>
#include <pce/cdrt.h>
#include <pce/cls.h>
#include <pce/pce.h>
#include "crypto-eip/crypto-eip.h"
#include "crypto-eip/ddk-wrapper.h"
#include "crypto-eip/internal.h"
#include "crypto-eip/debugfs.h"

struct mtk_crypto mcrypto;
struct device *crypto_dev;
struct mtk_crypto_priv *priv;
static struct device *eth_dev;
static u32 saved_clock_force[2];
static struct net_device *offload_devs[MTK_MAX_DEVS];
static bool offload_available[MTK_MAX_DEVS];

void crypto_eth_write(u32 reg, u32 val)
{
        writel(val, mcrypto.eth_base + reg);
}

bool mtk_crypto_supported_dev(struct net_device *dev)
{
        unsigned int i;

        for (i = 0; i < MTK_MAX_DEVS; i++)
                if (dev && dev == offload_devs[i] && READ_ONCE(offload_available[i]))
                        return true;
        return false;
}

u32 mtk_crypto_ppe_get_num(struct net_device *dev)
{
        unsigned int i;

        for (i = 0; i < MTK_MAX_DEVS; i++)
                if (dev == offload_devs[i])
                        return i == MTK_GMAC2_ID ? PSE_PORT_PPE1 :
                               i == MTK_GMAC3_ID ? PSE_PORT_PPE2 : PSE_PORT_PPE0;
        return PSE_PORT_PPE0;
}

static const struct xfrmdev_ops mtk_xfrmdev_ops = {
        .xdo_dev_state_add = mtk_xfrm_offload_state_add,
        .xdo_dev_state_delete = mtk_xfrm_offload_state_delete,
        .xdo_dev_state_free = mtk_xfrm_offload_state_free,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
        .xdo_dev_state_update_stats = mtk_xfrm_update_stats,
#else
        .xdo_dev_state_update_curlft = mtk_xfrm_update_stats,
#endif
        .xdo_dev_offload_ok = mtk_xfrm_offload_ok,
        .xdo_dev_packet_xmit = mtk_xfrm_packet_xmit,
        .xdo_dev_policy_add = mtk_xfrm_offload_policy_add,
        .xdo_dev_policy_delete = mtk_xfrm_offload_policy_delete,
        .xdo_dev_policy_free = mtk_xfrm_offload_policy_free,
};

void mtk_crypto_enable_ipsec_dev_features(struct net_device *dev)
{
        ASSERT_RTNL();
        WRITE_ONCE(dev->xfrmdev_ops, mtk_crypto_xfrmdev_ops());
        dev->features |= NETIF_F_HW_ESP;
        dev->hw_features |= NETIF_F_HW_ESP;
        dev->hw_enc_features |= NETIF_F_HW_ESP;
        netdev_change_features(dev);
}

void mtk_crypto_disable_ipsec_dev_features(struct net_device *dev)
{
        ASSERT_RTNL();
        if (dev->xfrmdev_ops != mtk_crypto_xfrmdev_ops())
                return;
        dev->features &= ~NETIF_F_HW_ESP;
        dev->hw_features &= ~NETIF_F_HW_ESP;
        dev->hw_enc_features &= ~NETIF_F_HW_ESP;
        dev->wanted_features &= ~NETIF_F_HW_ESP;
        netdev_change_features(dev);
        WRITE_ONCE(dev->xfrmdev_ops, NULL);
}

static int mtk_crypto_netdev_event(struct notifier_block *nb,
                                 unsigned long event, void *ptr)
{
        struct net_device *dev = netdev_notifier_info_to_dev(ptr);
        struct xfrm_params_list *list = mtk_xfrm_params_list_get();
        struct mtk_xfrm_params *params;
        unsigned int i;

        if (event == MTK_FE_START_RESET || event == MTK_FE_STOP_TRAFFIC) {
                for (i = 0; i < MTK_MAX_DEVS; i++)
                        if (offload_devs[i] == dev)
                                break;
                if (i == MTK_MAX_DEVS)
                        return NOTIFY_DONE;
                /* FE recovery calls mtk_stop directly, without NETDEV_DOWN.
                 * Do not reuse a transform after its FE tables were reset.
                 */
                spin_lock_bh(&list->lock);
                list_for_each_entry(params, &list->list, node)
                        params->dead = true;
                spin_unlock_bh(&list->lock);
                return NOTIFY_DONE;
        }
        if (event != NETDEV_UNREGISTER)
                return NOTIFY_DONE;
        for (i = 0; i < MTK_MAX_DEVS; i++)
                if (offload_devs[i] == dev)
                        WRITE_ONCE(offload_available[i], false);
        spin_lock_bh(&list->lock);
        list_for_each_entry(params, &list->list, node) {
                if (params->xs->xso.dev == dev) {
                        params->dead = true;
                        foe_clear_crypto_entry(params->cdrt->idx);
                }
        }
        spin_unlock_bh(&list->lock);
        /* Keep ops valid until the XFRM notifier has retired its states.
         * Every active SA pins this module until state_free finishes.
         */
        return NOTIFY_DONE;
}

static struct notifier_block mtk_crypto_netdev_notifier = {
        .notifier_call = mtk_crypto_netdev_event,
};

static struct notifier_block mtk_crypto_cls_notifier = {
        .notifier_call = mtk_xfrm_cls_notify,
};

static const struct mtk_crypto_net_ops mtk_crypto_net_ops = {
        .owner = THIS_MODULE,
        .xfrm = &mtk_xfrmdev_ops,
        .rx = mtk_crypto_decrypt_sec_path_setup,
        .tx = mtk_crypto_tx_prepare,
};

static void mtk_crypto_dts_deinit(void)
{
        unsigned int i;

        for (i = 0; i < MTK_MAX_DEVS; i++) {
                if (offload_devs[i])
                        dev_put(offload_devs[i]);
                offload_devs[i] = NULL;
                offload_available[i] = false;
        }
        iounmap(mcrypto.eth_base);
        iounmap(mcrypto.crypto_base);
        put_device(eth_dev);
        put_device(crypto_dev);
        eth_dev = NULL;
        crypto_dev = NULL;
}

static int __init mtk_crypto_dts_init(void)
{
        struct platform_device *pdev, *epdev;
        struct device_node *node, *enode;
        struct resource res;
        unsigned int i, found = 0;
        int ret = -ENODEV;

        node = of_find_compatible_node(NULL, NULL, HWPAL_PLATFORM_DEVICE_NAME);
        if (!node)
                return -ENODEV;
        pdev = of_find_device_by_node(node);
        of_node_put(node);
        if (!pdev)
                return -EPROBE_DEFER;
        crypto_dev = &pdev->dev;
        if (!crypto_dev->driver || !crypto_dev->driver->owner ||
            !strstr(module_name(crypto_dev->driver->owner), "crypto_eip_ddk"))
                goto err;
        if (of_address_to_resource(crypto_dev->of_node, 0, &res))
                goto err;
        /* DDK owns this resource. Share its MMIO window without claiming it
         * a second time, and release our mapping on every unload/failure.
         */
        mcrypto.crypto_base = ioremap(res.start, resource_size(&res));
        if (!mcrypto.crypto_base) {
                ret = -ENOMEM;
                goto err;
        }
        enode = of_parse_phandle(crypto_dev->of_node, "eth", 0);
        if (!enode)
                goto err;
        epdev = of_find_device_by_node(enode);
        of_node_put(enode);
        if (!epdev)
                goto err;
        eth_dev = &epdev->dev;
        mcrypto.eth = platform_get_drvdata(epdev);
        if (!eth_dev->driver || !mcrypto.eth ||
            !of_device_is_compatible(eth_dev->of_node, "mediatek,mt7988-eth"))
                goto err;
        if (of_address_to_resource(eth_dev->of_node, 0, &res))
                goto err;
        mcrypto.eth_base = ioremap(res.start, resource_size(&res));
        if (!mcrypto.eth_base) {
                ret = -ENOMEM;
                goto err;
        }
        mcrypto.ppe_num = mcrypto.eth->soc->ppe_num;
        rtnl_lock();
        for (i = 0; i < MTK_MAX_DEVS; i++) {
                struct net_device *dev = mcrypto.eth->netdev[i];

                if (!dev)
                        continue;
                if (dev->reg_state != NETREG_REGISTERED || dev->xfrmdev_ops) {
                        ret = -EBUSY;
                        break;
                }
                dev_hold(dev);
                offload_devs[i] = dev;
                offload_available[i] = true;
                found++;
        }
        rtnl_unlock();
        if (i == MTK_MAX_DEVS && found)
                return 0;
err:
        mtk_crypto_dts_deinit();
        return ret;
}

static void mtk_crypto_hw_deinit(void)
{
        mtk_ddk_pec_deinit();
        writel(saved_clock_force[0], mcrypto.crypto_base + EIP197_FORCE_CLK_ON);
        writel(saved_clock_force[1], mcrypto.crypto_base + EIP197_FORCE_CLK_ON2);
}

static int __init mtk_crypto_eip_init(void)
{
        unsigned int i;
        int ret;

        if (!mtk_pce_is_ready())
                return -EPROBE_DEFER;
        ret = mtk_crypto_dts_init();
        if (ret)
                return ret;
        saved_clock_force[0] = readl(mcrypto.crypto_base + EIP197_FORCE_CLK_ON);
        saved_clock_force[1] = readl(mcrypto.crypto_base + EIP197_FORCE_CLK_ON2);
        writel(~0U, mcrypto.crypto_base + EIP197_FORCE_CLK_ON);
        writel(~0U, mcrypto.crypto_base + EIP197_FORCE_CLK_ON2);
        ret = mtk_ddk_pec_init();
        if (ret)
                goto err_maps;
        ret = mtk_crypto_register_nf_hooks();
        if (ret)
                goto err_hw;
        ret = register_netdevice_notifier(&mtk_crypto_netdev_notifier);
        if (ret)
                goto err_nf;
        ret = mtk_pce_register_cls_notifier(&mtk_crypto_cls_notifier);
        if (ret)
                goto err_notifier;
        ret = mtk_crypto_net_register(&mtk_crypto_net_ops);
        if (ret)
                goto err_cls;
        rtnl_lock();
        for (i = 0; i < MTK_MAX_DEVS; i++)
                if (offload_available[i] && offload_devs[i]->reg_state == NETREG_REGISTERED)
                        mtk_crypto_enable_ipsec_dev_features(offload_devs[i]);
                else
                        WRITE_ONCE(offload_available[i], false);
        rtnl_unlock();
        mtk_crypto_debugfs_init();
        CRYPTO_INFO("EIP inline ready; XFRM offload remains opt-in per SA\n");
        return 0;
err_cls:
        mtk_pce_unregister_cls_notifier(&mtk_crypto_cls_notifier);
err_notifier:
        unregister_netdevice_notifier(&mtk_crypto_netdev_notifier);
err_nf:
        mtk_crypto_unregister_nf_hooks();
err_hw:
        mtk_crypto_hw_deinit();
err_maps:
        writel(saved_clock_force[0], mcrypto.crypto_base + EIP197_FORCE_CLK_ON);
        writel(saved_clock_force[1], mcrypto.crypto_base + EIP197_FORCE_CLK_ON2);
        mtk_crypto_dts_deinit();
        return ret < 0 ? ret : -EIO;
}

static void __exit mtk_crypto_eip_exit(void)
{
        unsigned int i;

        /* Live states hold a module reference, so this is reached only after
         * XFRM state_free has fenced and retired every hardware context.
         */
        mtk_crypto_net_unregister(&mtk_crypto_net_ops);
        rtnl_lock();
        for (i = 0; i < MTK_MAX_DEVS; i++)
                if (offload_devs[i])
                        mtk_crypto_disable_ipsec_dev_features(offload_devs[i]);
        rtnl_unlock();
        unregister_netdevice_notifier(&mtk_crypto_netdev_notifier);
        mtk_pce_unregister_cls_notifier(&mtk_crypto_cls_notifier);
        mtk_crypto_debugfs_exit();
        mtk_crypto_unregister_nf_hooks();
        mtk_crypto_hw_deinit();
        mtk_crypto_dts_deinit();
}

module_init(mtk_crypto_eip_init);
module_exit(mtk_crypto_eip_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek EIP-197 inline XFRM driver");
MODULE_AUTHOR("MediaTek Inc.; MedyMa");
MODULE_IMPORT_NS(CRYPTO_INTERNAL);
