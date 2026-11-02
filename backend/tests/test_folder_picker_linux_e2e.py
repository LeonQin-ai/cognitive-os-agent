"""Exercise the Linux folder picker through HTTP with a KDE-only desktop."""
import json,os,socket,subprocess,tempfile,time,urllib.parse,urllib.request
from pathlib import Path
if os.name!='posix' or __import__('sys').platform=='darwin':raise SystemExit(0)
root=Path(__file__).resolve().parents[1];binary=root/os.environ.get('COA_BUILD_DIR','build')/'cognitive-os-agent'
with tempfile.TemporaryDirectory(prefix='coa-linux-picker-') as tmp:
 state=Path(tmp);scripts=state/'bin';scripts.mkdir()
 selected=state/'selected';selected.mkdir()
 dialog=scripts/'kdialog';dialog.write_text('#!/bin/sh\nprintf "%s\\n" "$COA_PICKER_SELECTED"\n',encoding='utf8');dialog.chmod(0o755)
 state.joinpath('cognitive-os-agent.json').write_text('{"llm.provider":"mock"}',encoding='utf8')
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 with (root/'build'/'linux-folder-picker.log').open('w') as log:
  proc=subprocess.Popen([str(binary),'--state',tmp,'--workspace',tmp,'serve',str(port)],cwd=root,env={**os.environ,'PATH':str(scripts)+':/usr/bin:/bin','DISPLAY':':fixture','KDE_FULL_SESSION':'true','COA_PICKER_SELECTED':str(selected)},stdout=log,stderr=log)
  try:
   def api(path,body=None):
    req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=5) as response:return json.load(response)
   for _ in range(100):
    try:api('/v1/scheduler');break
    except OSError:assert proc.poll() is None;time.sleep(.05)
   result=api('/v1/workspaces/browse',{})
   assert result['path']==str(selected),result
   directory=api('/v1/workspaces/directories?path='+urllib.parse.quote(str(state),safe=''))
   assert directory['path']==str(state) and 'selected' in directory['directories'],directory
   print('PASS: Linux HTTP folder picker uses available KDE dialog')
  finally:proc.terminate();proc.wait(timeout=5)
