"""#218: actual nested-path recovery, without answer/intent keyword matching."""
import json,os,socket,subprocess,tempfile,threading,time,urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
class Model(BaseHTTPRequestHandler):
 calls=0;mode='';work=None
 def log_message(self,*args):pass
 def do_POST(self):
  self.rfile.read(int(self.headers['Content-Length']));n=Model.calls;Model.calls+=1
  if n==0:name,args='file_read',{'path':'framework/storage/module.h'}
  elif n==1:name,args='file_read',{'path':'generated/tree/framework/storage/module.h' if Model.mode=='nested' else 'generated/unrelated/module.h' if Model.mode=='basename' else str(Model.work.parent/'outside/framework/storage/module.h')}
  else:name,args='complete_task',{'status':'complete','requires_tools':False,'answer':'Analysis grounded in the successfully read source.'}
  raw=json.dumps({'choices':[{'message':{'content':'','tool_calls':[{'id':'call','type':'function','function':{'name':name,'arguments':json.dumps(args)}}]},'finish_reason':'tool_calls'}]}).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
model=ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=model.serve_forever,daemon=True).start()
try:
 for mode in ('nested','basename','outside'):
  Model.mode=mode;Model.calls=0
  with tempfile.TemporaryDirectory(prefix='coa-relocated-') as folder:
   base=Path(folder);work=base/'work';work.mkdir();Model.work=work;state=base/'state';state.mkdir()
   for path in (work/'generated/tree/framework/storage/module.h',work/'generated/unrelated/module.h',base/'outside/framework/storage/module.h'):
    path.parent.mkdir(parents=True,exist_ok=True);path.write_text('struct PersistedState { int version; };')
   (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixture','llm.api_key':'fixture','llm.base_url':f'http://127.0.0.1:{model.server_port}','reasoning.max_rounds':6}))
   with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
   def req(path,body=None):
    r=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(r,timeout=5) as response:return json.load(response)
   with (build/('relocated-'+mode+'.log')).open('w',encoding='utf8') as log:
    p=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',str(state),'--workspace',str(work),'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    try:
     for _ in range(100):
      try:req('/v1/scheduler');break
      except OSError:assert p.poll() is None;time.sleep(.05)
     tid=req('/v1/chat',{'message':'Analyze the persistence source','session':mode})['id'];end=time.monotonic()+15
     while True:
      task=req('/v1/tasks/'+str(tid))
      if task['status'] not in ('QUEUED','RUNNING'):break
      assert time.monotonic()<end;time.sleep(.03)
     assert task['status']==('DONE' if mode=='nested' else 'FAILED'),task
     assert task['steps'][0]['ok']==0 and task['steps'][1]['ok']==1
    finally:p.terminate();p.wait(timeout=5)
 print('PASS #218: verified full relative-path recovery; unrelated basename and other repository remain failures')
finally:model.shutdown();model.server_close()
