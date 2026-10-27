#!/usr/bin/env python3
"""Exercise the actual RXDMAD parser; kernel DMA services are modeled."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def definition(text, pattern):
    match = re.search(pattern, text)
    assert match, pattern
    start = text.index('{', match.end())
    depth = 0
    for end in range(start, len(text)):
        depth += (text[end] == '{') - (text[end] == '}')
        if not depth:
            return text[match.start():end + 1]
    raise ValueError(pattern)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--source', type=Path)
args = parser.parse_args()
package = Path(__file__).resolve().parent.parent
hwifi = package / 'src/wlan_hwifi'
source = args.source or hwifi / 'bus/mtk_pci_dma.c'
text = source.read_text()
header = (hwifi / 'bus/mtk_pci_dma.h').read_text()
bits = '\n'.join(line for line in header.splitlines()
                 if line.startswith('#define RXDMAD_') or
                 line.startswith('#define DMA_ADDR_H_SHIFT'))
enum = definition((hwifi / 'bus/mtk_rro.h').read_text(), r'enum IND_REASON\s*') + ';'
info = definition((hwifi / 'bus.h').read_text(), r'struct mtk_bus_rx_info\s*') + ';'
helper = definition(text, r'static inline bool\s+pci_dma_ind_reason_hw_path_ok\([^)]*\)')
actual_parser = definition(text, r'static int\s+pci_dma_rxdmad_parser\([^)]*\)')

prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint32_t __le32;
typedef unsigned long dma_addr_t;
#define BIT(n) (1UL << (n))
#define GENMASK(h,l) ((~0UL << (l)) & (~0UL >> (8 * sizeof(unsigned long) - 1 - (h))))
#define GET_FIELD(f,r) (((r) & f##_MASK) >> f##_SHIFT)
#define READ_ONCE(x) (x)
#define le32_to_cpu(x) (x)
#define mtk_dbg(...) ((void)0)
enum { RXDMAD_NON_DROP = 0, RXDMAD_VER_ZREO_DROP = 1 };
struct sk_buff;
struct pci_dma_desc { __le32 dw0, dw1, dw2, dw3; };
'''
services = r'''
struct hw_rro_cfg { struct { u32 rxdmad_reason_cnt[IND_REASON_MAX]; } rro_dbg; };
struct pci_trans { void *rro_cfg; };
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
'''
cases = r'''
int main(void)
{
    for (unsigned reason = 0; reason < 256; reason++)
        CHECK(pci_dma_ind_reason_hw_path_ok(reason) ==
              (reason == 0 || reason == 3 || reason == 9));

    for (unsigned ver = 0; ver < 2; ver++)
    for (unsigned reason = 0; reason < 16; reason++)
    for (unsigned ppe = 0; ppe < 2; ppe++)
    for (unsigned pn = 0; pn < 2; pn++)
    for (unsigned legacy_drop = 0; legacy_drop < 4; legacy_drop++)
    for (unsigned have_cfg = 0; have_cfg < 2; have_cfg++) {
        struct hw_rro_cfg cfg = {0};
        struct pci_trans trans = { .rro_cfg = have_cfg ? &cfg : NULL };
        int packet;
        struct mtk_bus_rx_info rx = {
            .pkt = &packet, .hw_path = 1, .ppe_entry = 2, .csrn = 3,
            .pn_chk_fail = 1, .repeat_pkt = 1, .old_pkt = 1, .drop = 1,
            .hw_rro = 1, .wifi_frag = 1, .eth_hdr_ofst = 14
        };
        struct pci_dma_desc desc = {
            .dw0 = 0x12345678,
            .dw1 = (ver ? RXDMAD_VER_MASK : 0) |
                   (pn ? RXDMAD_PN_CHK_FAIL_MASK : 0) |
                   (legacy_drop & 1 ? RXDMAD_RXD_DROP_MASK : 0) |
                   (legacy_drop & 2 ? RXDMAD_TO_HOST_A_MASK : 0) |
                   (1500U << RXDMAD_SDL0_SHIFT) | RXDMAD_LS0_MASK,
            .dw2 = (reason << RXDMAD_IND_REASON_SHIFT) |
                   (42U << RXDMAD_TOKEN_ID_SHIFT) | 1U,
            .dw3 = (ppe ? RXDMAD_PPE_VLD_MASK : 0) |
                   (1234U << RXDMAD_PPE_ENTRY_SHIFT) |
                   (7U << RXDMAD_CSRN_SHIFT) | RXDMAD_F_MASK
        };
        bool failed_pn = ver && (pn || reason == 13);
        bool dropped = !ver && legacy_drop;
        CHECK(!pci_dma_rxdmad_parser(&trans, &desc, &rx));
        CHECK(rx.hw_path == (ppe && !failed_pn && !dropped &&
                            (reason == 0 || reason == 3 || reason == 9)));
        CHECK(rx.pn_chk_fail == failed_pn);
        CHECK(rx.repeat_pkt == (ver && reason == 1));
        CHECK(rx.old_pkt == (ver && reason == 2));
        CHECK(rx.drop == (dropped ? RXDMAD_VER_ZREO_DROP : RXDMAD_NON_DROP));
        CHECK(rx.ppe_entry == (ppe ? 1234 : 0));
        CHECK(rx.csrn == (ppe ? 7 : 0));
        CHECK(rx.id == 42 && rx.len == 1500 && !rx.more && rx.ip_frag);
#ifdef CONFIG_HWIFI_64BIT_SUPPORT
        CHECK(rx.sdp0 == 0x112345678UL);
#else
        CHECK(rx.sdp0 == 0x12345678UL);
#endif
        CHECK(rx.pkt == &packet && rx.hw_rro && rx.wifi_frag && rx.eth_hdr_ofst == 14);
        for (unsigned i = 0; i < IND_REASON_MAX; i++) {
#ifdef CONFIG_HWIFI_DBG
            CHECK(cfg.rro_dbg.rxdmad_reason_cnt[i] == (have_cfg && ppe && i == reason));
#else
            CHECK(cfg.rro_dbg.rxdmad_reason_cnt[i] == 0);
#endif
        }
    }
    printf("PASS %u assertions: actual RXDMAD parser, all 16 reason codes, PN/drop/PPE/reuse combinations\n", checks);
    return 0;
}
'''
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
unit = prefix + bits + '\n' + enum + '\n' + info + '\n' + services + helper + actual_parser + cases
(out / 'parser.c').write_text(unit)
commands = []
for debug in (False, True):
    for dma64 in (False, True):
        name = f'parser-dbg{int(debug)}-dma64{int(dma64)}'
        flags = (['-DCONFIG_HWIFI_DBG'] if debug else []) + (
            ['-DCONFIG_HWIFI_64BIT_SUPPORT'] if dma64 else [])
        argv = ['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', *flags, str(out / 'parser.c'), '-o', str(out / name)]
        for command in (argv, [str(out / name)]):
            result = subprocess.run(command, capture_output=True, text=True)
            commands.append(dict(argv=command, exit=result.returncode,
                                 stdout=result.stdout, stderr=result.stderr))
            if result.returncode:
                break
receipt = dict(scope=__doc__, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
               commands=commands)
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
for command in commands:
    print(json.dumps({k: command[k] for k in ('exit', 'stdout', 'stderr')}))
raise SystemExit(0 if len(commands) == 8 and all(c['exit'] == 0 for c in commands) else 1)
