# BPI-R4 BE14 接入 UniFi Network

`be14-ucg` 在 R4 上向指定控制器的 `/inform` 发起连接。UCG 通过 E7 的型号代码 `UAPA697` 和系统 ID `a697` 识别这台三频 AP。设备名和管理 MAC 来自 R4。设备页的版本字段用于协议兼容，R4 的固件由 ImmortalWrt 更新。软件包随 WEXT 固件安装，首次启用需要指定控制器地址。

## 启用采用

在 R4 的 `/etc/config/be14_ucg` 中设置控制器的实际 IP 和 R4 已有的管理网络。下面的地址为示例。

```uci
config agent 'main'
        option enabled '1'
        option inform_url 'http://192.0.2.1:8080/inform'
        option management_network 'lan'
```

执行 `/etc/init.d/be14-ucg enable` 和 `/etc/init.d/be14-ucg restart`。在 UniFi Network 的设备列表中选择这台 R4 的 MAC 并采用。控制器如果要求通过 SSH 采用，请使用高级采用和 R4 已有的 SSH 管理凭据。自定义 SSH 端口可填入 `option sshd_port`。首次采用应在受信任的管理网络进行。更改控制器 IP 前需在维护窗口重新配对。采用密钥保存在 R4 的 `/etc/be14-ucg/state.json`，服务重启和保留配置升级都会沿用它。

已按 U7 Pro 采用的 R4 可以在原设备记录上升级代理。UniFi Network 10.6.109 实测会更新射频和客户端数据，但保留首次采用时的型号。需要重建为 E7 时，可在维护窗口停止代理，在 UCG 移除旧设备，然后在 R4 本地备份并移开配对文件。再次启动代理后，按原来的管理 MAC 重新采用。UCG 会创建新的设备记录。

## 在 UniFi 查看 Wi-Fi 客户端

在 UniFi 的「设置 → WiFi」中，将与 R4 实际 SSID 同名的 WLAN 分配给这台 AP，并选择对应频段。例如，查看连接 `ImmortalWrt-5G` 的终端时，使用 5 GHz。UCG 下发配置后，代理按频段和完整 SSID 匹配 WLAN 编号，再通过 `iwinfo assoclist` 逐个 BSSID 上报关联终端。这个过程只读取 R4 的无线配置。页面可以显示终端 MAC、信号和速率。R4 能解析主机信息时还会上报 IP 和主机名。

在 R4 执行 `be14-ucg-status` 可以核对匹配结果。每行显示本机 SSID、关联终端数和 WLAN 匹配状态。`no matching assignment` 表示已收到控制器的配置，但在该频段没有与本机 SSID 同名的可用 WLAN。把同名 WLAN 分配给这台 E7，保存后再次检查，状态应变为 `matched`。若显示 `awaiting controller WLAN settings`，代理还在等待已配对控制器的 WLAN 配置。

代理上报每个活动 BSSID 的运行信道、可读取的运行带宽和无线接口累计收发字节。状态里的 `netifd` 显示已应用的带宽配置，`operating MHz` 显示驱动读数，`?` 表示暂时无法读取。UniFi 根据连续两次字节读数计算流量速率。对于 6 GHz，要在 UCG 看到该频段的终端，请把同名的 6 GHz WLAN 分配给这台 AP，并选择与 R4 一致的 WPA3 个人或增强开放式加密。

要在 UCG 查看每台终端使用的应用，可以把一个测试 SSID 在 R4 上桥接到独立 VLAN，在上联网口放行该标签，并由 UCG 提供 DHCP 和默认网关。R4 对这个 VLAN 保留终端源地址，UCG 的「流量识别」即可分析经过网关的连接。在 UCG 按测试手机的 IP 核对应用和流量。R4 上的代理隧道若把流量封装到外部节点，UCG 在这条路径上看到的是隧道端点。[UniFi 流量识别说明](https://help.ui.com/hc/en-us/articles/12570783535383-UniFi-Gateway-Traffic-and-Device-Identification)

## 在 LuCI 自动下发 WLAN

在 UCG「设置 → WiFi」创建使用原生网络的独立 SSID，广播 AP 选已采用的 E7。在 R4「MTK → UniFi → 设置 → WLAN 配置下发」选择「自动下发 · 独立 AP」，选中该 WLAN 和频段。每个频段可以新建独立 AP，也可以选择已停用的备用 AP。保存后，测试终端通过 R4 的上联网桥向 UCG 获取 DHCP 地址。

自动模式按实际广播的 SSID 和频段上报客户端。UCG 撤下 WLAN 或启用 BE14 无法执行的接入策略（例如 MAC 白名单、定时开放或带标签的 VLAN）时，代理会停用对应的托管 AP。恢复支持的策略后会重新启用。原有主 SSID 继续使用本地配置。

## 手动绑定测试 WLAN

在 UCG 创建一个普通、无 VLAN 标签的个人 WLAN，使用控制器实际下发的 WLAN ID。控制器可能使用数字编号或 24 位十六进制 ObjectId。在 R4 上选择已运行的独立测试 AP。其 UCI 节名须以 `be14_ucg_` 开头，`mode` 为 `ap`，`mldgroup` 显式为 `0`，并接到预定的测试网络。可用 `l1util list` 查看真实无线设备的 UCI 名称。例如 `be14_ucg_five` 可以绑定在已改名的 5 GHz 设备上。

在同一个 `config agent 'main'` 段加入实际的 WLAN ID 和测试 AP 的节名，然后重启服务。每个频段可以列一个节，同一 WLAN 在 UCG 上也要启用相应频段。

```uci
        option wlan_id '0123456789abcdef01234567'
        list section 'be14_ucg_five'
```

支持通过 GCM 或经证书验证的 HTTPS 下发测试 SSID、PSK/SAE 口令、PMF、隐藏和客户端隔离，6 GHz 支持 SAE 和 OWE。当前管理范围为一个指定 WLAN 的独立 AP。VLAN、访客门户、MLO 组、控制器远程刷机和全站无线电参数在此版本的管理范围以外。代理只向 UCG 发送 `iwinfo` 实测到的活动 SSID。配置变化后由 netifd 重载，无线回读超时会恢复先前的绑定配置。日志可用 `logread -e be14-ucg` 查看。

## 实机验收

1. 在 Cloud Gateway Fiber 的 UniFi Network 11.1.50 上，确认 R4 的管理 MAC 从「等待采用」变为「在线」。
2. 重启 R4，确认相同设备记录自动在线，并显示 2.4、5、6 GHz 三个无线电。
3. 在 UCG 修改绑定的测试 SSID。用另一台支持对应频段的终端扫描并连接新名称，核对 BSSID 和连接频段。R4 上可用 `ubus call network.wireless status` 确定接口，再用 `iwinfo <接口名> info` 交叉检查。
