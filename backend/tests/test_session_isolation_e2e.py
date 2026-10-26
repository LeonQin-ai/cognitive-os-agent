"""#222 task snapshots: concurrent routes, permission/thinking isolation, no global switch."""
import json,os,socket,subprocess,tempfile,threading,time,urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build');seen=[];entered=threading.Event();release=threading.Event()
class Model(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));model=body['model'];seen.append((model,self.headers.get('Authorization')))
  if model=='model-a':entered.set();assert release.wait(10)
  message={'content':'','tool_calls':[{'id':'c','type':'function','function':{'name':'complete_task','arguments':json.dumps({'status':'complete','requires_tools':False,'answer':'Selected '+model})}}]}
  raw=json.dumps({'choices':[{'message':message,'finish_reason':'tool_calls'}]}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
model=ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=model.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory(prefix='coa-session-isolation-') as folder:
  state=Path(folder);(state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'global-default','llm.api_key':'default-fixture','llm.base_url':f'http://127.0.0.1:{model.server_port}'}))
  with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
  def req(path,body=None):
   r=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
   with urllib.request.urlopen(r,timeout=10) as response:return json.load(response)
  with (build/'session-isolation.log').open('w',encoding='utf8') as log:
   p=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',folder,'--workspace',folder,'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
   try:
    for _ in range(100):
     try:req('/v1/scheduler');break
     except OSError:assert p.poll() is None;time.sleep(.05)
    for letter in ('a','b'):req('/v1/routes',{'name':'route-'+letter,'provider':'openai','model':'model-'+letter,'api_key':'fixture-'+letter,'base_url':f'http://127.0.0.1:{model.server_port}'})
    a=req('/v1/chat',{'session':'a','message':'Describe yourself','route':'route-a','permission_mode':'ask','thinking':True})['id'];assert entered.wait(5)
    b=req('/v1/chat',{'session':'b','message':'Describe yourself','route':'route-b','permission_mode':'full','thinking':False})['id']
    release.set()
    for tid,model_name,permission,thinking in ((a,'model-a',1,True),(b,'model-b',0,False)):
     end=time.monotonic()+15
     while True:
      task=req('/v1/tasks/'+str(tid))
      if task['status'] not in ('RUNNING','QUEUED'):break
      assert time.monotonic()<end;time.sleep(.02)
     assert task['status']=='DONE' and task['model']==model_name and task['permission_mode']==permission and task['thinking']==thinking,task
     assert 'api_key' not in json.dumps(task) and 'fixture-a' not in json.dumps(task)
    assert req('/v1/config/llm')['model']=='global-default','local model selection changed global provider'
    assert ('model-a','Bearer fixture-a') in seen and ('model-b','Bearer fixture-b') in seen
    assert 'fixture-a' not in (state/'journal/tasks.jsonl').read_text(encoding='utf8'),'credentials leaked to journal'
   finally:release.set();p.terminate();p.wait(timeout=5)
 print('PASS #222: concurrent immutable routes/credentials, permission/thinking isolation and unchanged global model')
finally:model.shutdown();model.server_close()
