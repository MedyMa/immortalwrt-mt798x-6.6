# MT7988 TOPS/NPU port to ImmortalWrt 24.10

Development candidate on `mt7988-npu-tops-24.10`, base
`5efd97189fbf0b81b644f654e7fae0fedb237299`. This source submission has not
been flashed or hardware verified. Current official firmware/API inputs are pinned below.
The existing source/build branches, MTK/WiFi versions, BE14 software shadow,
owner accounting, two CPU/WiFi functional patches and 8192 MiB setting remain.
Only standard BPI-R4 receives the new board include; no Pro/25.12 build.

## Fixed provenance

* MediaTek `mtk-openwrt-feeds` c21f3b3617be249a1408a5e9daa66d402dd555bb:
  `feed/kernel/mtk_npu`, `feed/kernel/pce`, and its Filogic 24.10 HNAT overlay.
* Read-only MedyMa 25.12 reference 37f792d69b550eec684c54679c4ef49a0b2aa544:
  MT7988 power domains, tunnel descriptor and protocol device-path patches.
* traffic application base a70a103edac9129544b688ed5a6f1948ddf34075:
  `MedyMa/luci-app`, `Luci-app/luci-app-traffic` (candidate 1.2.3-r6).

This is MediaTek TOPS, not Airoha's similarly named WiFi NPU driver. Existing
hardware efuse checks remain in effect. Firmware statistic addresses are not
invented: this firmware release lacks the required address-discovery API.

## Implemented data path

* GRETAP, basic VXLAN and UDP L2TPv2 over unfragmented IPv4 are compiled into
  the same npu.ko as the HNAT adapter. Core/worker, callbacks, protocol types
  and resource removal therefore share a module lifetime. Flowblock, DPDK,
  multicast, PPTP, IPsec and ordinary routed GRE hardware backends stay off. The user confirmed
  their GRE usage is two-layer GRETAP.
* Encap learning saves the complete prepared INNER FOE and pins its device,
  CT identity and software sequence. Hardware stays UNBIND. On the outer
  packet, explicit kernel inner-header metadata and CT/NAT relation must
  agree. The backend must be firmware/PCE synchronized and keep its tunnel
  generation locked through actual PPE publication. A handled rejection
  never falls into the ordinary outer-flow memcpy.
* The outer LOCAL_OUT exception requires a fresh, matching saved INNER
  flow, current UNBIND/generation and CT/inner-header relation. Ordinary
  locally originated traffic keeps its original rejection. Final publication
  still repeats checks after outer NAT; the exception itself writes no FOE.
* Resolve outer next-hop MACs into a software path copy, never eth_hdr(skb),
  which may still be the encapsulated inner Ethernet frame. A missing/invalid
  neighbour leaves software forwarding untouched.
* For pure L2 bridges, consume only the existing pending decap marker in the
  MT7988 WHNAT-adjusted BR_PRE hook. This supplies decap learning even without
  an inner IPv4 netfilter traversal; normal bridge/hardware-decapped frames
  retain their original path. No bridge-netfilter sysctl is changed.
* Lock order: NPU tunnel -> HNAT entry -> software flow -> owner accounting.
  BH remains disabled through nested HNAT publication/retirement; process
  callers do not hold IRQs off across nested BH ledger locks. Software locks
  do not stop PPE hardware writes. The existing final live UNBIND/key gate
  remains immediately before descriptor publication, after accounting drain.
* Before firmware parameter replacement, retire old PPE bindings through
  HNAT's owner ledger. A failed firmware/PCE sync is retried at a bounded
  rate and is not publishable. Deleted slots are not immediately reused on
  a failed firmware delete ACK. Netdev references and pending shadow CT
  references are released; HNAT has bounded bucket sweeps and RCU callbacks.
* RX callbacks use synchronized registration and return held netdev references.
  An unknown/deleted hardware tunnel index is dropped, not assigned to an
  unrelated physical interface. Unsupported FIRST-LEARN formats keep normal
  software forwarding; this does not promise recovery of already malformed
  firmware output.
* L2TP gets scalar IDs under PPP unit/channel locks. No borrowed socket/session
  pointer escapes. PPP channel ABI changes, so all enabled channel providers
  must be rebuilt. Its native flow-check type/refcount and inner-header
  marker are adapted. Existing FOE-headroom relocation in skbuff.c is reused.
* PCE polling is bounded; queue initialization precedes worker/notifiers;
  firmware table addresses are validated; NPU readiness is published last.
  MT7988 TOPS0/1 domains and RX descriptor bits are in tracked kernel input.

## Accounting and application integration

INNER flow hardware bytes are attributed through the existing PPE MIB owner
ledger into the pinned inner conntrack. The outer skb's CT is never replaced.
The uncertain hardware tail at invalidation remains an unknown-loss event;
this is not a claim of lossless accounting or exact per-application totals.

WAN counters remain the overall total, and conntrack deltas remain application
attribution. TOPS host counters are setup/events, not bytes: never add them to
WAN/CT totals. No new broad endpoint/port exclusion is made: software/outer
CT association has not been proven precise enough to change attribution.

The `mtk-tops-status` helper (0.1-r2, Architecture all) reads software state,
CT accounting enable state, the HNAT software ledger, and `/proc/npu/tunnels`.
Reads use timeout 2 and max 1024/32/4096/8192 bytes, respectively; missing timeout,
truncation and missing nodes are explicit coverage. No debugfs mount, all_entry,
MIB transaction, service, settings write or counter reset is performed.
The root-only tunnel node copies at most 32 entries under individual locks,
records generation/endpoints/sync status, and performs no hardware I/O. It is
not an atomic whole-table hardware snapshot or a packet forwarding proof.

traffic candidate exposes optional `getNpuStatus` RPC/read ACL and a 30-second
TOPS status pane. The helper command has an outer timeout of 12 seconds and a
32768-byte JSON size check. Missing providers preserve existing page behavior.
The existing flow collector and total/rate calculations are unchanged.

## Current staged scope and activation

The staged MT7988 release defconfig now selects the backend and its autoload
packages by default, as requested. The backend can still be explicitly disabled.
This working candidate is not yet ready for the expanded full requirement:
IPv6 outer tunnel support remains unresolved in the public SDK/firmware inputs.
`MTK_NPU_24_10_TUNNEL_BACKEND=y`, kmod-npu and kmod-npu-nf_hnat select the
combined backend. GRETAP/VXLAN/L2TPv2 are the candidate protocol selections.
`mt7988-tunnels.config` records the corresponding firmware-build selections
for GRETAP/VXLAN/L2TPv2; these selections are now also staged in the MT7988
release defconfig. It is not an installed-device settings file. Runtime dependencies include the actual firmware, GRE/VXLAN/PPPoL2TP,
PCE, HNAT, generic PPP and the bounded status helper.
Native package metadata correctly emits CONFIG_MTK_PPP_TUNNEL_INFO=y when PPP
is selected. The native kmod-l2tp package also selects CONFIG_L2TP_V3=y and
l2tp_netlink. Those kernel providers have been built and linked; this does
not add a TOPS L2TPv3 backend. The board Ethernet/RX/power changes require a matching firmware
build; changing a vermagic/hash or forcing old kernel IPKs is not supported.

First encap binding supports non-GSO inner IPv4 TCP, init_net and a physical
MT7988 Ethernet/SFP egress, optionally behind one 802.1Q VLAN and/or a connected
PPPoE channel. Native channel/ndo callbacks provide the peer MAC, session and
VLAN identity. The PPE receives session/VID/PCP actions; the firmware parameter
ABI is unchanged. DSA, bridge and WiFi outer paths remain outside this scope.
This is a compiled hardware-path implementation, not an on-device packet result.
The native VLAN flag test now uses BIT(DEV_PATH_VLAN), rejecting a second tag
instead of overwriting the one VLAN field in the native path.

Host-only tunnel context pins both logical underlay and physical egress devices.
A changed session, VLAN or PCP advances the tunnel epoch and retires old PPE
bindings before re-publication. Device down/unregister/address/MTU/upper/carrier
changes retire matching context; all device references are released at deletion.
Software locks do not freeze PPP sessions or PPE hardware; final descriptor
state/key and accounting generation checks retain their existing boundary.
Outer IPv4 options/fragments, GRE options/key/sequence, VXLAN extensions,
L2TPv3, sequence/offset/compressed PPP and custom L2TP/VXLAN ports are outside
this candidate. Protocol/settings changes must currently down/recreate the
virtual device before reuse; arbitrary in-place L2TP option changes have no
complete offload invalidation interface in this port. These are deployment
limits, not statements that these cases have passed hardware validation.

The candidate includes the published PPPQ/WED uplink fix and the software-RX
metadata/headroom fix at that base. It does not restore the earlier stale-tag
behavior or replace the existing WiFi native patch queue.
The 24.10 core package directly selects the PPP and HNAT providers. Conditional
provider selection through a feature which depends on the NPU integration
package produced recursive Kconfig dependencies in this tree. Protocol and
accounting-helper dependencies retain their conditional package expressions.
PCE has a platform guard and no artificial HNAT self-dependency; its module
does not import HNAT symbols. SCAN_DEPS tracks the included package/config files
so edits cannot leave stale package metadata. The HNAT integration package owns the
PPP scalar-snapshot kernel option. The optional absent vendor scripts directory
is skipped during installation rather than producing an ignored error.

Native strict MODPOST has passed for PCE, combined NPU, HNAT, PPP, L2TP,
GRE/IP/UDP tunnels and VXLAN using the isolated kernel's real vmlinux exports.
Selected-source native queue replay covers 28 files and 90 selected patches;
it is not a claim that a complete firmware or every kernel queue file was
prepared. Existing ETH/power/R4 DTB evidence is retained where inputs match.
Cloud receipts give exact commands, config hashes, input hashes and artifacts.
Twenty-two targeted cases use extracted real LOCAL_OUT/inner-tuple/neighbour/
decap functions with dependencies shimmed; the old local-out and neighbour
functions reproduce both regressions. These are not real kernel/firmware
packet executions. Final MODPOST was also run directly without either -n
(missing-file bypass) or -w (unresolved warnings), against copied exact native
composite objects and authentic current exports.
Native package recipes have also produced PCE/NPU/firmware and the affected
HNAT, GRE, VXLAN, L2TP and PPP packages. Their native kernel dependency is
`6.6.94~2ea691deadead2daa581c0fd4c6ef8b0-r1`; it is specific to the staged
full release validation configuration and must not be forced onto a different installed kernel.
The isolated metadata view still warns about some unrelated userspace feed
providers being absent. These receipts validate the named targets and this
recorded kernel configuration; they do not establish the ABI of a future
complete release image or validate every release package.
Core CRC/conntrack are built in for that profile; their metadata packages
therefore legitimately contain no loadable module. Autoload/combined-backend
packages are also metadata packages, not additional duplicate drivers.

`tests/test-accounting.py --output /path/outside/source` extracts the actual
candidate V3 FOE/owner structures, commit/drain/ledger/publish/invalidate and
matching functions and runs them with CT/MMIO/locking services shimmed.
It currently checks 267 assertions, including pinned inner-CT attribution,
no double ledger consumption, missing CT accounting extension, legal ownerless
publication, allocation failure, drain-time key/state/type/generation changes,
dying CT, and invalidation races. GCC AddressSanitizer and UndefinedBehaviorSanitizer
are enabled. This supplements the 22 route/tuple/neighbour cases above;
neither harness simulates actual TOPS firmware packet processing.
`tests/test-status-read.py --output /path/outside/source` additionally checks
the exact bounded shell reader for complete/empty/truncated/missing input,
missing timeout tools and a blocked FIFO (terminated after two seconds).

The actual staged MT7988 release defconfig selects NPU/PCE/HNAT/firmware and
autoload by default; 8192 MiB and the original MTK firmware selections remain.
The full native kernel configuration was derived from that actual release input,
including its pre-existing TC/bridge/filesystem selections. Necessary built-in
objects were re-linked with the kernel-only vmlinux_o target and fresh exports
were generated by native modpost, then independently checked with strict -M -E.
Affected providers and NPU/PCE packages were rebuilt against that actual table.
Original build .config files and old artifacts remain preserved. No world,
global modules, firmware image target or device operation was performed.

`tests/test-underlay.py --kernel <prepared-kernel> --output <external-directory>`
extracts the actual native VLAN, egress/PPE/lease and firmware-table bounds
functions, the real tunnel allocator/free paths and the enclosing offload
transaction. It reverses the tracked one-line VLAN patch on a temporary copy for
the baseline reproduction. The current 392 assertions include old-VLAN, IRQ/BH
lock-context and allocation-rollback failures. An early rejected new INIT slot
is released, while queued firmware synchronization and existing slots are kept.
The outer offload transaction holds BH disabled with IRQs enabled before calling
HNAT retirement; lock nesting does not re-enable BH inside an outer held lock. Its separate required-ipv6 mode intentionally fails against the
actual SDK matcher: that failure records an unmet requirement, not IPv6 success.
`tests/test-startup.py --output <external-directory>` executes actual startup
and removal control flow under injected dependency failures. Its 109 assertions
cover 26 init failure points, absent resources/firmware and supplier deferral.
Kernel APIs, component internals and firmware are shimmed, so neither test is
a board boot, packet delivery or component resource-leak proof.
`tests/test-config.py --topdir <prepared-config-view> --defconfig <release-input>
--output <external-directory>` runs the native configuration graph for the
profile, implicit default, backend opt-out and protocol opt-out. It rejects
dependency errors even when make exits zero, and restores the original config.
All four cases now pass without dependency errors; the resulting default
configuration is byte-identical to the one used for the module builds.

## IPv6 outer requirement: still blocked

Official main was rechecked at c21f3b3617be249a1408a5e9daa66d402dd555bb.
The public driver has an IPv6 parameter enum/structure and DIP filter support,
but npu_params.c still marks IPv6 decapsulation/matching TODO, there is no IPv6
network parser implementation, and Ethernet encapsulation accepts IPv4 only.
The imported firmware is version 1.1.0.0, API 0; its metadata lists protocol
names but does not establish IPv6 outer encapsulation/checksum support.
Public searches found no confirmed replacement implementation. The official
git01 mirror branch was additionally checked at
526e416e20e4bdad15e2209e902dd65b406083b6: its feed/kernel directory contains
crypto-eip, fips-debugfs and pce, and the known NPU source paths return 404.
Its complete, non-truncated 5509-entry Git tree contains TOPS tools and kernel
integration patches, but no NPU driver/IPv6 network implementation to import.
The alternate MediaTek Gitiles endpoint returned HTTP 403, without a bypass attempt.

Completing this requirement needs the IPv6 host/kernel tunnel paths together
with a confirmed matching firmware ABI/implementation, including mandatory IPv6
UDP checksums. No capability bit or firmware response is invented, and simply
removing IPv4 rejection is not a valid implementation. Ordinary IPv6 PPE routing
is a separate existing feature and has not been disabled by this tunnel gate.
This submission covers the explicitly bounded IPv4 backend and preserves safe
software forwarding for unsupported formats. It does not complete the requested
IPv6 outer hardware feature. Native build/regression results do not establish
production hardware readiness or successful IPv6 outer packet delivery.

Before production activation, verify firmware boot, actual bidirectional
packets, down/recreate/reload, unsupported-format fallback and byte deltas
against a single authoritative transfer. Compilation and a software synced
entry alone cannot prove that the hardware forwarded or accounted traffic.
