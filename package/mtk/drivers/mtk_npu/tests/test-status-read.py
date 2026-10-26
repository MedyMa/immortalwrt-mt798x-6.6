#!/usr/bin/env python3
"""Exercise the actual bounded shell reader with ordinary files and a FIFO."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess,tempfile,time

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,required=True)
args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[5]
source=root/'package/utils/mtk-tops-accounting/files/mtk-tops-status'
text=source.read_text();start=text.index('bounded_read() {');end=text.index('\n}\n',start)+3
function=text[start:end]
script=out/'bounded-read.sh'
script.write_text(function+'''\nret=0
bounded_read "$1" "$2" || ret=$?
printf '%s\\n%s\\n%s\\n' "$ret" "$READ_COVERAGE" "$READ_VALUE"
''')
with tempfile.TemporaryDirectory(prefix='tops-status-') as tmp:
    t=Path(tmp);(t/'complete').write_text('abcd');(t/'empty').touch();(t/'long').write_text('x'*1024)
    os.mkfifo(t/'blocked');(t/'no-tools').mkdir()
    cases=[('complete',t/'complete',4,'complete','abcd',False),
           ('empty',t/'empty',4,'complete','',False),
           ('truncated',t/'long',4,'truncated','',False),
           ('missing',t/'absent',4,'missing','',False),
           ('no-timeout',t/'complete',4,'no_timeout_tool','',True),
           ('blocked',t/'blocked',4,'read_exit_124','',False)]
    rows=[]
    for name,path,limit,coverage,value,no_tools in cases:
        env=os.environ.copy()
        if no_tools:env['PATH']=str(t/'no-tools')
        cmd=['/bin/sh',str(script),str(path),str(limit)]
        started=time.monotonic();r=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=4);wall=time.monotonic()-started
        lines=r.stdout.splitlines();expected_rc='0' if coverage=='complete' else '1'
        ok=r.returncode==0 and not r.stderr and lines[:2]==[expected_rc,coverage] and '\n'.join(lines[2:])==value
        if name=='blocked':ok=ok and 1.7<wall<3.8
        rows.append({'case':name,'argv':cmd,'exit':r.returncode,'wall_seconds':round(wall,3),'coverage':lines[1] if len(lines)>1 else None,'pass':ok,'stderr_bytes':len(r.stderr)})
        assert ok,rows[-1]
receipt={'scope':'verbatim bounded_read shell function; synthetic local files/FIFO, no device/sysfs/debugfs operations','source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'function_sha256':hashlib.sha256(function.encode()).hexdigest(),'cases':rows}
(out/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps({'pass':len(rows),'cases':rows}))
