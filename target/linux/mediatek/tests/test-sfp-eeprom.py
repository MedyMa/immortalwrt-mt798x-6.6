#!/usr/bin/env python3
"""Execute native SFP probe/state-machine functions; I2C and kernel services modeled.

The old source must fail the same regression. This does not test a physical module.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--kernel', type=Path, required=True)
p.add_argument('--source', type=Path, help='Candidate sfp.c; defaults to the prepared kernel')
p.add_argument('--baseline', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
args = p.parse_args()
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
source_path = args.source or args.kernel / 'drivers/net/phy/sfp.c'
source = source_path.read_text()
baseline = args.baseline.read_text()
header = (args.kernel / 'include/linux/sfp.h').read_text()
types = header[header.index('struct sfp_eeprom_base {'):header.index('struct fwnode_handle;')]


def function(text, name):
    m = re.search(r'(?m)^static (?:unsigned int|bool|int|void) ' + name + r'\([^;{]*\)\s*(?=\{)', text)
    assert m, name
    opening = text.index('{', m.end())
    mask = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                  lambda m: ' ' * len(m[0]), text, flags=re.S)
    depth = 0
    for end in range(opening, len(text)):
        depth += (mask[end] == '{') - (mask[end] == '}')
        if not depth:
            return text[m.start():end + 1] + '\n'
    raise ValueError(name)


prefix = r'''
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint16_t __be16;
typedef uint32_t __be32;
#define __packed __attribute__((packed))
#define __LITTLE_ENDIAN_BITFIELD
#define BIT(n) (1U<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min(a,b) ((a)<(b)?(a):(b))
#define msecs_to_jiffies(x) (x)
#define KERN_ERR ""
#define DUMP_PREFIX_OFFSET 0
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define mutex_lock(l) ((void)(l))
#define mutex_unlock(l) ((void)(l))
#define fallthrough __attribute__((fallthrough))
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);} } while (0)
static void *memchr_inv(const void *p,int c,size_t n) {
 const u8 *b=p;for(size_t i=0;i<n;i++)if(b[i]!=(u8)c)return (void *)(b+i);return NULL;
}
'''
services = r'''
enum {MDIO_I2C_NONE,MDIO_I2C_C45,MDIO_I2C_MARVELL_C22};
struct sfp;
struct sfp_quirk {void (*fixup)(struct sfp *);};
struct sff_data {bool (*module_supported)(const struct sfp_eeprom_id *);};
struct sfp {
 struct sfp_eeprom_id id;size_t i2c_block_size;void *dev,*sfp_bus;
 const struct sff_data *type;const struct sfp_quirk *quirk;
 void *gpio[GPIO_MAX];int st_mutex,reprobe,poll;
 unsigned module_t_start_up,module_t_wait,phy_t_retry,state_ignore_mask;
 bool no_rollball_bridge,sm_reprobe_enabled,have_a2,sm_reprobing;
 unsigned mdio_protocol,state_hw_mask,state_hw_drive,rs_state_mask;
 unsigned sm_mod_state,sm_dev_state,sm_mod_tries_init,sm_mod_tries,sm_reprobe_tries,module_power_mW;
};
static struct sfp_eeprom_id image;
static unsigned attempts,unready_attempts,read_calls,ext_reads,inserts,lookups,writes,warnings,rtl_warnings;
static unsigned elapsed,last_timer;
static int read_failure;
static bool emulate_byte_only,erased;
static unsigned checksum_failures;
static void log_message(void *dev,const char *fmt,...) {
 (void)dev;warnings++;if(strstr(fmt,"Detected broken RTL8672"))rtl_warnings++;
}
#define dev_err log_message
#define dev_warn log_message
#define dev_info log_message
static void print_hex_dump(const char *l,const char *p,int d,int rows,int group,const void *b,size_t n,bool ascii) {
 (void)l;(void)p;(void)d;(void)rows;(void)group;(void)ascii;
 CHECK(n==sizeof(struct sfp_eeprom_base));CHECK(b!=NULL);
}
static int sfp_read(struct sfp *s,bool a2,u8 addr,void *buf,size_t len) {
 CHECK(!a2);read_calls++;
 if(addr==0 && s->i2c_block_size==SFP_EEPROM_BLOCK_SIZE)attempts++;
 if(addr==SFP_CC_BASE+1){ext_reads++;memcpy(buf,&image.ext,len);return len;}
 CHECK(addr==0 && len==sizeof(image.base));
 if(attempts<=unready_attempts){
  if(read_failure<0)return read_failure;
  if(read_failure>0)return len-1;
  memset(buf,erased?0xff:0,len);return len;
 }
 memcpy(buf,&image.base,len);
 if(attempts<=checksum_failures)((struct sfp_eeprom_base *)buf)->cc_base++;
 if(emulate_byte_only && s->i2c_block_size>1){
  for(size_t i=0;i<len;i++)if(i%s->i2c_block_size)((u8 *)buf)[i]=0;
 }
 return len;
}
static int sfp_write(struct sfp *s,bool a2,u8 addr,void *p,size_t n) {(void)s;(void)a2;(void)addr;(void)p;writes++;return n;}
static void mdelay(unsigned t){(void)t;}
static int sfp_module_parse_power(struct sfp *s){s->module_power_mW=1000;return 0;}
static void sfp_module_parse_rate_select(struct sfp *s){(void)s;}
static const struct sfp_quirk *sfp_lookup_quirk(struct sfp *s,struct sfp_eeprom_id *id){(void)s;(void)id;lookups++;return NULL;}
static void sfp_sm_mod_next(struct sfp *s,unsigned state,unsigned delay){s->sm_mod_state=state;last_timer=delay;}
static void sfp_sm_set_timer(struct sfp *s,unsigned delay){(void)s;last_timer=delay;}
static void cancel_delayed_work(void *w){(void)w;}
static void mod_delayed_work(int w,void *p,int delay){(void)w;(void)p;(void)delay;}
static const int system_wq;
static int sfp_hwmon_insert(struct sfp *s){(void)s;return 0;}
static void sfp_sm_mod_remove(struct sfp *s){memset(&s->id,0,sizeof(s->id));}
static void sfp_module_remove(void *bus){(void)bus;}
static int sfp_sm_mod_hpower(struct sfp *s,bool on){(void)s;(void)on;return 0;}
static int sfp_module_insert(void *bus,const struct sfp_eeprom_id *id,const struct sfp_quirk *q){
 (void)bus;(void)q;inserts++;
 /* Model phylink refusing an identity with no link capabilities. */
 return memchr_inv(&id->base,0,sizeof(id->base))?0:-EINVAL;
}
'''
tests = r'''
static const struct sff_data supported={.module_supported=sfp_module_supported};
static void checksum(void){image.base.cc_base=sfp_check(&image.base,sizeof(image.base)-1);image.ext.cc_ext=sfp_check(&image.ext,sizeof(image.ext)-1);}
static struct sfp fresh(void) {
 attempts=unready_attempts=read_calls=ext_reads=inserts=lookups=writes=warnings=rtl_warnings=elapsed=last_timer=0;
 read_failure=0;emulate_byte_only=erased=false;checksum_failures=0;memset(&image,0,sizeof(image));
 image.base.phys_id=SFF8024_ID_SFP;image.base.phys_ext_id=SFP_PHYS_EXT_ID_SFP;
 image.base.connector=SFF8024_CONNECTOR_RJ45;image.base.extended_cc=SFF8024_ECC_10GBASE_T_SFI;
 memcpy(image.base.vendor_name,"OEM             ",16);memcpy(image.base.vendor_pn,"SFP-10G-T       ",16);
 checksum();struct sfp s={.type=&supported,.sm_dev_state=SFP_DEV_UP};return s;
}
static void run(struct sfp *s) {
 sfp_sm_module(s,SFP_E_INSERT);
 for(unsigned i=0;i<64 && s->sm_mod_state==SFP_MOD_PROBE;i++) {
  elapsed+=last_timer;sfp_sm_module(s,SFP_E_TIMEOUT);
 }
 CHECK(s->sm_mod_state!=SFP_MOD_PROBE);
}
int main(void) {
 _Static_assert(sizeof(struct sfp_eeprom_base)==64,"native SFF base ABI");
 _Static_assert(sizeof(struct sfp_eeprom_ext)==32,"native SFF extended ABI");
 struct sfp s=fresh();unready_attempts=1;run(&s);
 CHECK(s.sm_mod_state==SFP_MOD_PRESENT && attempts==2 && inserts==1);
 CHECK(s.mdio_protocol==MDIO_I2C_C45 && !memcmp(&s.id,&image,sizeof(image)));
 CHECK(!rtl_warnings && !writes);
 for(unsigned n=1;n<=20;n++) {
  s=fresh();unready_attempts=n;run(&s);
  CHECK(s.sm_mod_state==SFP_MOD_PRESENT && attempts==n+1 && inserts==1 && lookups==1);
  CHECK(s.mdio_protocol==MDIO_I2C_C45 && !rtl_warnings && !writes);
 }
 for(unsigned variant=0;variant<4;variant++) {
  s=fresh();unready_attempts=1000;erased=variant==1;read_failure=variant==2?-ETIMEDOUT:variant==3?1:0;run(&s);
  CHECK(s.sm_mod_state==SFP_MOD_ERROR && !inserts && !ext_reads && !lookups && !writes);
  CHECK(attempts==R_PROBE_RETRY_INIT+R_PROBE_RETRY_SLOW-1);
  CHECK(elapsed<=T_SERIAL+R_PROBE_RETRY_INIT*T_PROBE_RETRY_INIT+R_PROBE_RETRY_SLOW*T_PROBE_RETRY_SLOW);
  CHECK(!rtl_warnings);
 }
 s=fresh();unready_attempts=100;memset(&s.id,0xa5,sizeof(s.id));
 struct sfp_eeprom_id saved=s.id;CHECK(sfp_sm_mod_probe(&s,false)==-EAGAIN);
 CHECK(!memcmp(&saved,&s.id,sizeof(saved)) && !inserts && !ext_reads && !lookups && !writes);
 s=fresh();checksum_failures=1;run(&s);CHECK(s.sm_mod_state==SFP_MOD_PRESENT && attempts==2 && inserts==1);
 s=fresh();checksum_failures=1000;run(&s);
 CHECK(s.sm_mod_state==SFP_MOD_ERROR && attempts==21 && !ext_reads && !inserts && !lookups);
 for(unsigned variant=0;variant<3;variant++) {
  s=fresh();unready_attempts=3;erased=variant==0;read_failure=variant==1?-ETIMEDOUT:variant==2?1:0;run(&s);
  CHECK(s.sm_mod_state==SFP_MOD_PRESENT && attempts==4 && inserts==1 && !writes);
 }
 for(unsigned i=0;i<4;i++) {
  u8 codes[]={SFF8024_ECC_10GBASE_T_SFI,SFF8024_ECC_10GBASE_T_SR,SFF8024_ECC_5GBASE_T,SFF8024_ECC_2_5GBASE_T};
  s=fresh();image.base.extended_cc=codes[i];checksum();run(&s);
  CHECK(s.sm_mod_state==SFP_MOD_PRESENT && s.mdio_protocol==MDIO_I2C_C45 && attempts==1);
 }
 s=fresh();image.base.extended_cc=0;image.base.e1000_base_t=1;checksum();run(&s);
 CHECK(s.sm_mod_state==SFP_MOD_PRESENT && s.mdio_protocol==MDIO_I2C_MARVELL_C22);
 s=fresh();image.base.connector=SFF8024_CONNECTOR_LC;image.base.extended_cc=0;image.base.e10g_base_sr=1;checksum();run(&s);
 CHECK(s.sm_mod_state==SFP_MOD_PRESENT && s.mdio_protocol==MDIO_I2C_NONE && !memcmp(&s.id,&image,sizeof(image)));
 s=fresh();memset(image.base.vendor_name,0,sizeof(image.base.vendor_name));image.base.extended_cc=0;image.base.e10g_base_sr=1;checksum();run(&s);
 CHECK(s.sm_mod_state==SFP_MOD_PRESENT && s.mdio_protocol==MDIO_I2C_NONE);
 s=fresh();memcpy(image.base.vendor_name,"UBNT            ",16);memcpy(image.base.vendor_pn,"UF-INSTANT      ",16);
 image.base.phys_id=SFF8024_ID_SFF_8472;emulate_byte_only=true;checksum();run(&s);
 CHECK(s.sm_mod_state==SFP_MOD_PRESENT && s.i2c_block_size==1 && attempts==1 && !writes);
 const char *vendors[]={"COTSWORKS       ","JESS-LINK       ","SK-LINK         "};
 for(unsigned i=0;i<3;i++){s=fresh();memcpy(image.base.vendor_name,vendors[i],16);checksum();image.base.cc_base++;run(&s);CHECK(s.sm_mod_state==SFP_MOD_PRESENT && attempts==1);}
 s=fresh();image.base.phys_id=image.base.phys_ext_id=0;checksum();run(&s);
 CHECK(s.sm_mod_state==SFP_MOD_PRESENT && attempts==1); /* nonblank legacy zero-ID rule */
 s=fresh();image.base.phys_id=0xfe;checksum();run(&s);CHECK(s.sm_mod_state==SFP_MOD_ERROR && attempts==1 && !inserts);
 s=fresh();unready_attempts=100;sfp_sm_module(&s,SFP_E_INSERT);sfp_sm_module(&s,SFP_E_TIMEOUT);
 CHECK(s.sm_mod_state==SFP_MOD_PROBE);sfp_sm_module(&s,SFP_E_REMOVE);CHECK(s.sm_mod_state==SFP_MOD_EMPTY && !inserts);
 unready_attempts=0;run(&s);CHECK(s.sm_mod_state==SFP_MOD_PRESENT && inserts==1 && !writes);
 printf("PASS %u checks: actual SFP probe/module state machine; transient/permanent blank, erased, short and failed reads, corrupt checksum, copper/optical, byte-I/O and legacy quirks; I2C/kernel services modeled\n",checks);
 return 0;
}
'''


def unit(text):
    enums = text[text.index('enum {'):text.index('static const char  * const mod_state_strings')]
    defines = '\n'.join(re.findall(r'(?m)^#define (?:T_START_UP|T_WAIT|T_PHY_RETRY|T_SERIAL|T_HPOWER_LEVEL|T_PROBE_RETRY_INIT|R_PROBE_RETRY_INIT|T_PROBE_RETRY_SLOW|R_PROBE_RETRY_SLOW|R_REPROBE|SFP_EEPROM_BLOCK_SIZE)\s+[^\n]+', text))
    names = ['sfp_module_supported', 'sfp_check', 'sfp_id_needs_byte_io',
             'sfp_id_has_valid_vendor_name', 'sfp_cotsworks_fixup_check',
             'sfp_module_parse_sff8472', 'sfp_sm_mod_probe', 'sfp_sm_module']
    if 'static bool sfp_id_is_unready(' in text:
        names.insert(4, 'sfp_id_is_unready')
    return prefix + types + enums + defines + '\n' + services + '\n'.join(function(text, n) for n in names) + tests


commands = []
for name, text, expected in [('before', baseline, 1), ('after', source, 0)]:
    path = out / (name + '.c')
    path.write_text(unit(text))
    cc = ['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
          '-Wno-unused-function', '-Wno-unused-parameter', '-fsanitize=address,undefined',
          '-fno-omit-frame-pointer', str(path), '-o', str(out / name)]
    for argv, want in [(cc, 0), ([str(out / name)], expected)]:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=30)
        commands.append(dict(argv=argv, exit=r.returncode, expected_exit=want, stdout=r.stdout, stderr=r.stderr))
        if r.returncode != want:
            break
    if commands[-1]['exit'] != commands[-1]['expected_exit']:
        break
receipt = dict(scope=__doc__, source=str(source_path), source_sha256=hashlib.sha256(source.encode()).hexdigest(), baseline_sha256=hashlib.sha256(baseline.encode()).hexdigest(), commands=commands)
(out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
print(json.dumps(commands, indent=2))
raise SystemExit(0 if len(commands) == 4 and all(c['exit'] == c['expected_exit'] for c in commands) else 1)
