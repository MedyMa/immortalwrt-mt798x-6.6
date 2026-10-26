# BPI-R4 BE14 firmware RRO compatibility workaround

## Scope

Native patch `976-bpi-r4-be14-bypass-rro.patch` selects `ALL_BYPASS` (3) for
firmware RX reordering when the device tree matches `bananapi,bpi-r4` and
the MT7990 chip option is 2 (dual PCIe, 6/2+5). Selection occurs in the
existing chip option initialization; the existing capability transfer and
firmware initialization send that value to the MCU.

The native RRO-disabled and forced-disabled branches retain their selection.
Other board/layout selections retain their original policy. This changes the
firmware RX reorder mode, preserving the existing WED/PPE configuration,
ring layout and TX-free routing. It does not enable WHNAT globally or change
the profile's existing WHNAT setting. The native runtime `rro_bypass` control
remains available.

This is an empirically verified workaround. The underlying firmware/host
RRO defect has not been identified, and this patch is not an upstream RRO
algorithm fix.

## Device evidence, 2026-10-26

Device firmware: `24.10.6 r0-93979d1`, kernel `6.6.94`, standard BPI-R4,
MT7990 option 2, RX path 3, HIF TX descriptor version 33. The same WiFi client
and speedtest node were used. WHNAT remained 1 in all three controlled rounds:

| Firmware RRO mode | Native runtime control | Reported upload |
| --- | --- | --- |
| ALL_BYPASS (3) | `rro_bypass=1` | 210 Mbps |
| ALL_NOT_BYPASS (1) | `rro_bypass=0` | 22.65 Mbps |
| ALL_BYPASS (3), repeated | `rro_bypass=1` | 203 Mbps |

Driver logs confirmed each mode and WHNAT selection. Existing PPE snapshots
captured matching TCP upload BIND entries and payload-sized packet counters
during the bypass round, as well as upload BIND entries in the slow control.
This supports retained PPE hardware activity; a BIND entry alone is not a
proof that every byte followed hardware. No diagnostic kernel hooks or new
trace counters were added. The earlier, untimed 200 Mbps result is excluded.
Test windows restored WHNAT=0 and firmware RRO mode 1 afterward.

## Validation boundaries

The native package preparation applied patches 010, 974, 975 and 976. The
prepared MT7990 source matched the tested candidate byte for byte. A real
MT7990 object was compiled with the package's expanded native kernel flags
for both the original and patched sources. The only added undefined symbol
was `of_machine_is_compatible`, present in the same build's kernel exports.
Full WiFi provider module linkage is left to firmware CI.

`tests/test-bpi-r4-rro.py --output <external-directory>` applies the actual
patch with zero fuzz and runs the actual option-selection function under
ASan/UBSan with kernel services modeled. The old policy fails the required
board/layout case; the patched policy passes 642 assertions covering 64
board/layout/RRO combinations, including disabled and forced-disabled RRO.

The runtime workaround was exercised on the installed firmware. A new
firmware containing this startup patch still needs cold boot, warm boot,
WiFi reload and sustained/multiple-client validation. No download figures
were supplied for these rounds, so they do not establish download performance.
No unrelated MT7990 layout, other WiFi chip or IPv6 throughput claim is made.

After upgrading, the standard board/option-2 startup log should report
`rro_bypass_type = 3`. `iwpriv rai0 show mac_cap` exposes the selected host
capability in the kernel log. Check the separate WHNAT state explicitly;
RRO bypass is not a WHNAT enable switch.
