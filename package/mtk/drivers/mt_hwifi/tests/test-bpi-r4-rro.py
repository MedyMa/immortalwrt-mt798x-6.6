#!/usr/bin/env python3
"""Apply the native patch and test actual MT7990 option selection with services modeled."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
package = Path(__file__).resolve().parent.parent
root = package.parents[3]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
rel = 'wlan_hwifi/chips/mt7990/mt7990.c'
source = package / 'src' / rel
baseline = source.read_text() if source.exists() else subprocess.check_output(
    ['git', 'show', 'HEAD:package/mtk/drivers/mt_hwifi/src/' + rel], cwd=root, text=True)
copy = out / 'queue' / rel
copy.parent.mkdir(parents=True, exist_ok=True)
copy.write_text(baseline)
patch = package / 'patches/976-bpi-r4-be14-bypass-rro.patch'
apply = subprocess.run(['patch', '--batch', '--fuzz=0', '-p1', '-d', str(out / 'queue')],
                       input=patch.read_text(), capture_output=True, text=True)
if apply.returncode:
    raise RuntimeError(apply.stdout + apply.stderr)


def function(s):
    m = re.search(r'static int\s+mt7990_chip_option_set\(', s)
    assert m
    opening = s.index('{', m.end())
    depth = 0
    for i in range(opening, len(s)):
        depth += (s[i] == '{') - (s[i] == '}')
        if not depth:
            return s[m.start():i + 1] + '\n'
    raise ValueError('function boundary')


prefix = r'''
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char u8;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define Q_TX_DATA 1
#define HWIFI_FLAG_CHIP_OPTION 1
#define HWIFI_FLAG_IN_CHIP_RRO 2
#define ALL_BYPASS 3
#define dev_err(...) ((void)0)
struct pci_queue {unsigned q_attr,band_idx_bmp,q_type_bmp;};
struct mtk_bus_tx_ring_desc {unsigned band_idx_bmp,q_type_bmp;};
struct mtk_bus_desc {unsigned tx_ring_num;bool offload_disable;struct mtk_bus_tx_ring_desc tx_ring_desc[2];};
struct mtk_chip_desc {unsigned rx_path_type,max_ba_wsize_scene_mlo;bool force_rro_disable;
 unsigned rro_bypass_type,mld_dest_type,txfreedone_path;
 unsigned rro_disable_rro_bypass_type,rro_disable_mld_dest_type,rro_disable_txfreedone_path;};
struct mtk_chip_option_table {struct mtk_chip_desc chip_desc;struct mtk_bus_desc bus_desc_master;};
struct mtk_chip_drv {struct mtk_chip_option_table chip_opt_table[8];};
struct mtk_hw_dev {struct mtk_chip_drv *chip_drv;unsigned chip_opt,rro_mode;unsigned long flags;
 unsigned rx_path_type,max_ba_wsize_scene_mlo,rro_bypass_type,mld_dest_type,txfreedone_path;};
struct pci_trans {struct mtk_hw_dev *hw;unsigned txq_num;struct {struct pci_queue q;} txq[2];bool offload_disable;};
#define to_hw_dev(t) ((t)->hw)
static bool bpi_r4;
static bool of_machine_is_compatible(const char *s){CHECK(!strcmp(s,"bananapi,bpi-r4"));return bpi_r4;}
static void set_bit(unsigned n,unsigned long *flags){*flags|=1UL<<n;}
static unsigned ring_updates;
static void mt7990_chip_option_set_txrx_nums(struct mtk_hw_dev *h,struct mtk_bus_desc *b){(void)h;(void)b;++ring_updates;}
'''
tests = r'''
int main(void){
 for(unsigned board=0;board<2;board++)for(unsigned opt=0;opt<8;opt++)
 for(unsigned enabled=0;enabled<2;enabled++)for(unsigned forced=0;forced<2;forced++) {
  struct mtk_chip_drv drv={0};struct mtk_hw_dev hw={.chip_drv=&drv,.chip_opt=opt,.rro_mode=enabled};
  struct pci_trans trans={.hw=&hw,.txq_num=1,.txq={{{.q_attr=Q_TX_DATA}}}};
  struct mtk_chip_desc *d=&drv.chip_opt_table[opt].chip_desc;
  struct mtk_bus_desc *bus=&drv.chip_opt_table[opt].bus_desc_master;
  *d=(struct mtk_chip_desc){.rx_path_type=3,.max_ba_wsize_scene_mlo=512,.force_rro_disable=forced,
   .rro_bypass_type=1,.mld_dest_type=7,.txfreedone_path=0,
   .rro_disable_rro_bypass_type=3,.rro_disable_mld_dest_type=5,.rro_disable_txfreedone_path=1};
  bus->tx_ring_num=1;bus->offload_disable=false;
  bus->tx_ring_desc[0]=(struct mtk_bus_tx_ring_desc){.band_idx_bmp=6,.q_type_bmp=15};
  bpi_r4=board;unsigned before=ring_updates;CHECK(!mt7990_chip_option_set(&trans));
  bool off=!enabled||forced;
  CHECK(hw.rro_bypass_type==(off||(board&&opt==2)?3:1));
  CHECK(hw.rx_path_type==3&&hw.max_ba_wsize_scene_mlo==512);
  CHECK(hw.mld_dest_type==(off?5:7));CHECK(hw.txfreedone_path==(off?1:0));
  CHECK(!!(hw.flags&(1UL<<HWIFI_FLAG_IN_CHIP_RRO))==!off);
  CHECK(hw.flags&(1UL<<HWIFI_FLAG_CHIP_OPTION));CHECK(ring_updates==before+1);
  CHECK(trans.txq[0].q.band_idx_bmp==6&&trans.txq[0].q.q_type_bmp==15);
  CHECK(!trans.offload_disable);
 }
 printf("PASS %u assertions: actual MT7990 option selection, 64 board/layout/RRO combinations; services modeled\n",checks);
 return 0;
}
'''
commands = []
for label, text in [('baseline', baseline), ('candidate', copy.read_text())]:
    f = out / (label + '.c')
    f.write_text(prefix + function(text) + tests)
    cmd = ['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
           '-Wno-unused-function', '-fsanitize=address,undefined',
           '-fno-omit-frame-pointer', str(f), '-o', str(out / label)]
    for argv in [cmd, [str(out / label)]]:
        r = subprocess.run(argv, capture_output=True, text=True)
        commands.append(dict(label=label, argv=argv, exit=r.returncode,
                             stdout=r.stdout, stderr=r.stderr))
        if len(argv) > 1 and r.returncode:
            break
receipt = dict(scope=__doc__, patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),
               prepared_sha256=hashlib.sha256(copy.read_bytes()).hexdigest(),
               patch_stdout=apply.stdout, commands=commands)
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(commands, indent=2))
raise SystemExit(0 if [x['exit'] for x in commands] == [0, 1, 0, 0] else 1)
