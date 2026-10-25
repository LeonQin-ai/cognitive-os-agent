"""Actual Runtime: selected directories, durable automatic/manual Flow and failed chat state."""
import json,os,re,socket,subprocess,tempfile,threading,time,urllib.request,urllib.error
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build');counts={}
class Model(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));sys=body['messages'][0]['content']
  if '你是多 agent 编排器' in sys:
   message={'content':json.dumps([{'agent':'alpha','task':'Read marker.txt and copy it to result.txt'},{'agent':'alpha','task':'Summarize {{step1}}'}])}
  elif '综合各 agent' in sys:message={'content':'All nodes verified.'}
  else:
   text='\n'.join(m['content'] for m in body['messages'] if isinstance(m.get('content'),str));found=re.findall(r'## Execution workspace\n([^\n]+)',text);assert found
   directory=found[-1];n=counts.get(directory,0);counts[directory]=n+1
   if directory.endswith('failure'):name,args='complete_task',{'status':'blocked','requires_tools':False,'answer':'Missing evidence; task failed.'}
   elif n==0:name,args='file_read',{'path':'marker.txt'}
   elif n==1:name,args='file_write',{'path':'result.txt','copy_from':'marker.txt'}
   else:name,args='complete_task',{'status':'complete','requires_tools':False,'answer':'Verified selected directory '+directory}
   message={'content':'','tool_calls':[{'id':'call','type':'function','function':{'name':name,'arguments':json.dumps(args)}}]}
  raw=json.dumps({'choices':[{'message':message,'finish_reason':'tool_calls' if 'tool_calls' in message else 'stop'}]}).encode()
  self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
model=ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=model.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory(prefix='coa-flow-workspace-') as tmp:
  base=Path(tmp).resolve();state=base/'state';state.mkdir();folders=[]
  for name in ('fallback','manual','automatic','failure'):
   path=base/name;path.mkdir();(path/'marker.txt').write_bytes(('proof '+name).encode());folders.append(path)
  (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixture','llm.api_key':'fixture','llm.base_url':f'http://127.0.0.1:{model.server_port}','reasoning.max_rounds':6}),encoding='utf8')
  def portnum():
   with socket.socket() as sock:sock.bind(('127.0.0.1',0));return sock.getsockname()[1]
  port=portnum()
  def req(path,body=None):
   r=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
   with urllib.request.urlopen(r,timeout=10) as resp:return json.load(resp)
  def finish(tid):
   end=time.monotonic()+20
   while time.monotonic()<end:
    t=req(f'/v1/tasks/{tid}')
    if t['status'] not in ('RUNNING','QUEUED'):return t
    time.sleep(.03)
   raise AssertionError('task stalled')
  with (build/'flow-workspace-e2e.log').open('w',encoding='utf8') as log:
   def start():
    proc=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',str(state),'--workspace',str(base),'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    for _ in range(100):
     try:req('/v1/scheduler');return proc
     except OSError:assert proc.poll() is None;time.sleep(.05)
    raise AssertionError('startup timeout')
   proc=start()
   try:
    for path in folders:req('/v1/workspaces',{'path':str(path)})
    # No roster: the automatic single-agent fallback must inherit the directory too.
    fallback=req('/v1/orchestrate',{'task':'Read marker.txt and copy to result.txt','workspace':str(folders[0])})['id']
    assert finish(fallback)['status']=='DONE'
    req('/v1/agents',{'name':'alpha','role':'Read files and verify artifacts'})
    dag={'nodes':[{'id':'copy','agent':'alpha','task':'Read marker.txt and copy to result.txt'}]}
    manual=req('/v1/flows',{'flow':dag,'workspace':str(folders[1]),'input':'manual copy'})['id']
    auto=req('/v1/orchestrate',{'task':'Copy marker then summarize','workspace':str(folders[2])})['id']
    for tid,path in ((manual,folders[1]),(auto,folders[2])):
     task=finish(tid);assert task['status']=='DONE',task
     assert Path(task['workspace'])==path
    for path in folders[:3]:assert (path/'result.txt').read_bytes()==(path/'marker.txt').read_bytes()
    assert not (base/'result.txt').exists()
    records=req('/v1/flows/list')['records'];assert len(records)==3
    assert all(r['status']=='DONE' for r in records),records
    record=next(r for r in records if r['task_id']==auto);assert record['automatic'] and Path(record['workspace'])==folders[2]
    # An actually failed chat must retain status without browser storage.
    sid=req('/v1/chat/sessions',{'workspace':str(folders[3])})['id']
    fail=req('/v1/chat',{'message':'Inspect missing evidence','session':sid})['id'];assert finish(fail)['status']=='FAILED'
    assert next(s for s in req('/v1/chat/sessions') if s['id']==sid)['last_run']=={'id':fail,'status':'FAILED'}
    before=req('/v1/chat/history?session='+sid);assert any(r['id']==fail and r['status']=='FAILED' for r in before['runs'])
   finally:proc.terminate();proc.wait(timeout=5)
   # Seed an authentic journal shape larger than the old all-or-nothing cap;
   # HTTP history after startup must retain the final failure and all steps.
   trace=[{'stage':'failed','elapsed_ms':880000,'steps':[{'tool':'file_read','target':'evidence-'+str(i)+'x'*700,'ok':0} for i in range(100)]}]
   with (state/'journal/tasks.jsonl').open('a',encoding='utf8') as f:
    f.write(json.dumps({'id':999,'status':'FAILED','session':sid,'input':'long failure','output':'retained failure','trace':trace,'ts':1792930961000})+'\n')
   port=portnum();proc=start()
   try:
    assert next(s for s in req('/v1/chat/sessions') if s['id']==sid)['last_run']=={'id':999,'status':'FAILED'}
    after=req('/v1/chat/history?session='+sid);assert after['runs'][:-1]==before['runs'],'failed answer survives process restart'
    assert after['runs'][-1]['trace']==trace,'long failure timeline remains available'
    records2=req('/v1/flows/list')['records'];assert records2==records,'automatic and manual history survives restart'
    counts[str(folders[2])]=0
    resumed=req('/v1/flows/'+str(record['id'])+'/resume',{})['id'];task=finish(resumed)
    assert task['status']=='DONE' and Path(task['workspace'])==folders[2],task
    assert req('/v1/flows/list')['records'][0]['automatic']
    # Invalid directories must be rejected rather than running in the default.
    try:req('/v1/flows',{'flow':dag,'workspace':str(base/'missing')});raise AssertionError('invalid directory accepted')
    except urllib.error.HTTPError as e:assert e.code==400
   finally:proc.terminate();proc.wait(timeout=5)
 print('PASS: selected cwd for manual/auto/fallback, byte-copy evidence, durable records, resume directory and failed chat restart state')
finally:model.shutdown();model.server_close()
