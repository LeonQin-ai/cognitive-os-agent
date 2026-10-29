"""Isolated Windows desktop preference persistence over real Runtime HTTP."""
import json,os,socket,subprocess,tempfile,time,urllib.request,urllib.error
from pathlib import Path
root=Path(__file__).resolve().parents[1]
if os.name!='nt':raise SystemExit('Windows desktop preference test')
with tempfile.TemporaryDirectory() as folder:
 state=Path(folder);(state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'mock','auth.key':''}))
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 def api(method='GET',body=None,status=200):
  request=urllib.request.Request(f'http://127.0.0.1:{port}/v1/desktop/startup-animation',method=method,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
  try:response=urllib.request.urlopen(request,timeout=5)
  except urllib.error.HTTPError as error:response=error
  assert response.status==status,(response.status,response.read())
  return json.load(response)
 def start():
  proc=subprocess.Popen([str(root/'build/cognitive-os-agent.exe'),'--state',folder,'--workspace',folder,'serve',str(port)],cwd=root,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,env={**os.environ,'LOCALAPPDATA':folder},creationflags=subprocess.CREATE_NO_WINDOW)
  for _ in range(100):
   try:api();return proc
   except OSError:assert proc.poll() is None;time.sleep(.1)
  proc.terminate();proc.wait();raise TimeoutError('Runtime not ready')
 proc=start()
 try:
  assert api()=={'available':True,'enabled':True}
  assert api('PUT',{'enabled':False})['enabled'] is False
  api('PUT',{'enabled':'false'},400)
  assert api()['enabled'] is False
 finally:proc.terminate();proc.wait(timeout=10)
 proc=start()
 try:
  assert api()['enabled'] is False
  assert api('PUT',{'enabled':True})['enabled'] is True
  assert (state/'cognitive-os-agent/desktop-startup-animation').read_text()=='1'
 finally:proc.terminate();proc.wait(timeout=10)
 print('PASS: default enabled, strict boolean, save, restart persistence, re-enable; isolated user preference.')
