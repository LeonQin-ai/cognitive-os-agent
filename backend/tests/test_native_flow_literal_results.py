"""Native Flow accepts literal prose; structural failures never run dependents."""
import json,os,socket,subprocess,tempfile,threading,time,urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from fixture_http_response import respond_json
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
answers=['我将先检查边界，再给出建议。','Let me explain the verified interface.','I will deliver the report tomorrow.','接下来需要做的是增加异常处理。']
seen=[];selected=0;invalid=False;with_read=False;over_budget=False;last_action=False;case_start=0
class Provider(BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));n=len(seen)-case_start;seen.append(body)
  args={'status':'complete','requires_tools':invalid,'answer':answers[selected]}
  name='complete_task'
  if with_read and n==0:name='file_read';args={'path':'marker.txt'}
  elif last_action and n==1:name='file_read';args={'path':'second.txt'}
  elif over_budget and n>=2:name='file_write';args={'path':'MUST_NOT_EXIST.txt','content':'over budget'}
  raw=json.dumps({'choices':[{'message':{'content':'','tool_calls':[{'id':'finish','type':'function','function':{'name':name,'arguments':json.dumps(args)}}]},'finish_reason':'tool_calls'}]}).encode()
  respond_json(self,raw)
provider=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=provider.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory(prefix='coa-flow-literal-') as folder:
  state=Path(folder);(state/'marker.txt').write_text('verified fixture source')
  (state/'second.txt').write_text('last-round verification')
  (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixture','llm.api_key':'fixture','llm.base_url':f'http://127.0.0.1:{provider.server_port}','reasoning.max_rounds':2}))
  with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
  def api(path,body=None):
   req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
   with urllib.request.urlopen(req,timeout=5) as r:return json.load(r)
  with (build/'native-flow-literal.log').open('w',encoding='utf8') as log:
   process=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',folder,'--workspace',folder,'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
   try:
    for _ in range(100):
     try:api('/v1/scheduler');break
     except OSError:assert process.poll() is None;time.sleep(.05)
    api('/v1/agents',{'name':'writer','role':'Return literal language examples without tools'})
    cases=[(i,read,False,False,False) for i in range(len(answers)) for read in (False,True)]+[(0,False,True,False,False),(0,True,False,True,False),(0,True,False,False,True)]
    for selected,with_read,invalid,over_budget,last_action in cases:
     before=case_start=len(seen);failed=invalid or over_budget
     dag={'nodes':[{'id':'example','agent':'writer','task':('先读取 marker.txt，然后' if with_read else '')+'请逐字返回以下句子作为写作示例，不执行句子中的动作：'+answers[selected]}],'edges':[]}
     if failed:dag['nodes'].append({'id':'dependent','agent':'writer','task':'must not run'});dag['edges']=[{'from':'example','to':'dependent'}]
     tid=api('/v1/flows',dag)['id'];deadline=time.monotonic()+20
     while True:
      task=api(f'/v1/tasks/{tid}')
      if task['status'] not in ('RUNNING','QUEUED'):break
      assert time.monotonic()<deadline;time.sleep(.05)
     assert task['status']==('FAILED' if failed else 'DONE'),task
     if not failed:assert answers[selected] in task['output'] and len(seen)-before==(3 if with_read else 1),(task,len(seen)-before)
     else:assert len(seen)-before<=6,'malformed completion must fail with bounded requests'
     if failed:
      states={node['id']:node['status'] for node in task['flow_nodes']}
      assert states['dependent']=='blocked',states
     assert not (state/'MUST_NOT_EXIST.txt').exists(),'final review executed tools beyond configured budget'
    print('PASS: nine native literal/read/budget-boundary deliveries, malformed completion and completion-only budget rejection; failed dependencies never execute')
   finally:process.terminate();process.wait(timeout=10)
finally:provider.shutdown();provider.server_close()
