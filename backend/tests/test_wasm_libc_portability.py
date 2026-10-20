"""Compile the actual wasm libc with implicit declarations forbidden; test bounded lengths."""
import os,shlex,shutil,subprocess,tempfile
from pathlib import Path
backend=Path(__file__).resolve().parents[1]
zig=backend/'tools/zig/zig.exe'
cc=shlex.split(os.environ['CC']) if os.environ.get('CC') else [shutil.which('cc') or shutil.which('clang') or '']
if not cc[0]:cc=[str(zig),'cc']
with tempfile.TemporaryDirectory(prefix='coa-wasm-c11-') as tmp:
 root=Path(tmp)
 flags=['-std=c11','-Werror=implicit-function-declaration','-I'+str(backend/'third_party/wasm3')]
 subprocess.run(cc+flags+['-c',str(backend/'third_party/wasm3/m3_api_libc.c'),'-o',str(root/'libc.o')],check=True)
 source=root/'bounds.c'
 source.write_text('''#include "m3_strnlen.h"
#include <assert.h>
int main(void) {
 const char bounded[3]={'a','b','c'};
 assert(m3_libc_strnlen("",10)==0);
 assert(m3_libc_strnlen("hello",3)==3);
 assert(m3_libc_strnlen("hello",9)==5);
 assert(m3_libc_strnlen(bounded,sizeof bounded)==3);
 assert(m3_libc_strnlen((const char*)0,0)==0);
 return 0;
}
''',encoding='utf-8')
 exe=root/('bounds.exe' if os.name=='nt' else 'bounds')
 subprocess.run(cc+flags+[str(source),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print('PASS: actual wasm libc compiles as strict C11; empty, limited, terminated, unterminated and zero-limit lengths')
