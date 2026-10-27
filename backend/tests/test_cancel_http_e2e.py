"""#221: cancellation interrupts silent HTTP headers and body, including streaming."""
import json,os,socket,subprocess,tempfile,threading,time,urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build');entered=threading.Event();release=threading.Event();mode='headers';calls=0
class Model(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  global calls
  body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));calls+=1
  if mode=='body':self.send_response(200);self.send_header('Content-Type','text/event-stream' if body.get('stream') else 'application/json');self.send_header('Content-Length','100000');self.end_headers();self.wfile.flush()
  entered.set();release.wait(20)
model=ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=model.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory(prefix='coa-cancel-http-') as folder:
  state=Path(folder);(state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixture','llm.api_key':'fixture','llm.base_url':f'http://127.0.0.1:{model.server_port}','llm.timeout_ms':300000}))
  with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
  def req(method,path,body=None):
   r=urllib.request.Request(f'http://127.0.0.1:{port}'+path,method=method,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
   with urllib.request.urlopen(r,timeout=5) as response:return json.load(response)
  with (build/'cancel-http-e2e.log').open('w',encoding='utf8') as log:
   p=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',folder,'--workspace',folder,'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
   try:
    for _ in range(100):
     try:req('GET','/v1/scheduler');break
     except OSError:assert p.poll() is None;time.sleep(.05)
    req('POST','/v1/agents',{'name':'alpha','role':'Analyze'})
    for mode,stream in (('headers',False),('body',False),('headers',True),('body',True),('orchestrate',False))*3:
     entered.clear();before=calls
     if mode=='orchestrate':
      tid=req('POST','/v1/orchestrate',{'task':'Describe yourself'})['id']
     else:tid=req('POST','/v1/chat',{'message':'Describe yourself','session':mode+str(stream),'stream':stream})['id']
     assert entered.wait(5)
     started=time.monotonic();req('DELETE','/v1/tasks/'+str(tid))
     while True:
      task=req('GET','/v1/tasks/'+str(tid))
      if task['status'] not in ('RUNNING','QUEUED'):break
      assert time.monotonic()-started<3,'cancel remained blocked by model I/O';time.sleep(.02)
     assert time.monotonic()-started<3,'cancel terminal state exceeded 3s'
     assert task['status']=='CANCELLED' and calls==before+1,(task,calls,before)
     assert '任务已取消' in task['output'],task
     # The first terminal state is authoritative; it must not drift later.
     assert req('GET','/v1/tasks/'+str(tid))['status']=='CANCELLED',task
   finally:release.set();p.terminate();p.wait(timeout=5)
 print('PASS #221: cancel finishes within 3s during silent headers/body for regular and streaming requests; no retries')
finally:release.set();model.shutdown();model.server_close()
