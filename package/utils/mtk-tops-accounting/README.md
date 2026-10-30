# 24.10 HNAT 与 traffic 统计接口

普通转发由 PPE/HNAT 处理。NPU 就绪、五核利用率、隧道参数同步和
`npu_hnat_events` 均不能单独证明硬件转发；该事件节点统计主机资格检查与配置尝试。
驱动也未承诺为每个私有 HNAT 流设置 conntrack 的 `HW_OFFLOAD` 标志。

## 本轮修复

- IPv4 `pkt_type=0` 是 HNAPT 格式。只有动态 UNBIND 槽的整个四元组为零时，
  才允许通过已确认 CT 重建键；仍校验 skb、CT 方向、软暂存序号和 accounting 代次。
  非空槽保留严格元组与协议检查。
- 连接销毁、手动清槽和暂停前，统一账本尝试读取仍属于旧 owner 的最后一段硬件计数。
  CT 引用延迟释放，避免在 PPE/entry 锁下递归销毁连接。轮询及回收有槽数与服务时间预算。
- `GET_CTRZERO` 的字节与包数清零语义保持兼容，额外的 lifetime carry 位于单独分配的
  sidecar，避免 Linux 6.6 conntrack 扩展的 u8 长度限制。内存不足时不伪造 lifetime 数据。
- 可选、只读、仅 init_net 的 `nf_conntrack_closed` 保存最后 2048 个已结束连接。
  首次打开才启动记录；30 秒未读取即停记，恢复读取时通过 `gaps=` 告知采集缺口。
  它不调用 MIB、不增加逐包 hook、不清零计数。
- 利用率接口兼容原五核均值，同时输出各核数值；硬件型号与固件信息读取实际节点。

## 日志复核

- HNAT 收发热路径的 `trace_printk` 改为 `pr_debug_ratelimited`，默认不产生逐包日志；
  需要时可使用内核动态调试。debug_level 7 的相关绑定日志也限速。
- NPU 未就绪的利用率读取直接返回 `-EAGAIN`，避免正常启动/恢复期间反复打印。
  邮箱请求失败或无效利用率回复仍有带 core 与 errno 的限速告警；去掉上层重复报错。
  初始化、固件加载、watchdog、SER 恢复等错误日志保留。
- BE14 无参数 MAC 表每次模块加载只打印一次，以原子 bit 防止并发轮询重复打印。
  同步修复 `mt_hwifi` 和实际 WiFi7 MAC 构建使用的 `mt_wifi7` 两份源文件。
  显式参数的诊断查询仍可重复执行。该守卫通过宿主并发测试；本轮未编译完整 WiFi SDK。

## 编译与离线回归

使用经过 SHA256 核对的 Linux 6.6.159，按顺序准备 generic backport/pending/hack
与 mediatek 补丁，再叠加实际 files/files-6.6。复用 WiFi7 GCC 14.3.0 工具链，
验证 ARM64 `vmlinux`、HNAT、SFP/Aquantia、PCE 与含 HNAT/组播/隧道的 NPU 模块。
没有生成完整固件。

```sh
python3 scripts/tests/mtk-accounting-selftest.py --kernel-tree /path/to/prepared/linux-6.6.159
python3 scripts/tests/mtk-telemetry-selftest.py --jshn-bin /path/to/host/jshn --jshn-shell /path/to/jshn.sh
```

宿主测试执行实际函数和解析器，验证 IPv4 碰撞、空槽、状态变化、字节及包数反复清零、
64 位数值、五核报告及并发日志守卫。它们不能模拟或证明 MMIO、DMA 顺序和实机性能。

## 实机验收边界

在持续普通 IPv4 TCP/UDP、IPv6、BE14 双向上传下载和各受支持隧道负载下，核对：

1. 单次读取 `hnat/hnat_entry`，查看各 PPE 的 BIND 数量及目标流元组。
   使用实际负载前后账本快照，确认 `poll_reads` 和 `ct_written_bytes/packets` 增长。
2. 读取 `hnat/accounting` 的 `mismatch`、`unknown_loss`、`final_reads/final_errors`；
   使用 nlbwmon 清零后，核对同一 `traffic_id` 的 lifetime 字节和包数仍递增。
3. 核对短连接、连接退出、相同元组重建、端口/隧道删除及模块重载，确保不会串流归属。
4. 对齐 WAN 接口层、隧道内层 CT 与应用层口径；这些值不能相加。
5. 正常观察页面时日志量应稳定；故障及显式诊断的必要信息仍可见。

硬件自主复用相同元组及最终读取之后仍在途的计数尾量，需要实机验证。
`unknown_loss` 和 `lossless_hardware_tail_proven=false` 保留这些边界，不能宣称完全无损。
