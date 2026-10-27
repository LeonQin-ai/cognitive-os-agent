"""IM must use any free chat lane, and cancellation must interrupt lock waits."""
import json,os,socket,subprocess,tempfile,threading,time,urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from fixture_http_response import respond_json
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
binary=build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
gates={};entered={};group_calls=[];errors=[]
class Provider(BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  try:
   body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
   text='\n'.join(m['content'] for m in body['messages'] if isinstance(m.get('content'),str))
   current=text.rsplit('## Current request\n',1)[-1].split('\n## ',1)[0]
   blocker=next((key for key in entered if key in current),None)
   if blocker:
    entered[blocker].set();gates[blocker].wait(20);answer='Blocking fixture released.'
   elif 'You are alpha in an IM group' in current:
    group_calls.append(current);answer='GROUP_CAPACITY_OK [[DISCUSSION_DONE]]'
   else:answer='Session fixture completed.'
   raw=json.dumps({'choices':[{'message':{'content':'','tool_calls':[{'id':'done','type':'function','function':{'name':'complete_task','arguments':json.dumps({'status':'complete','requires_tools':False,'answer':answer})}}]},'finish_reason':'tool_calls'}]}).encode()
   respond_json(self,raw)
  except Exception as e:errors.append(repr(e));raise
with tempfile.TemporaryDirectory(prefix='coa-im-capacity-') as folder:
 work=Path(folder);provider=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=provider.serve_forever,daemon=True).start()
 (work/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'capacity-fixture','llm.base_url':f'http://127.0.0.1:{provider.server_port}','llm.api_key':'fixture','scheduler.workers':8,'reasoning.max_rounds':4}),encoding='utf-8')
 with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
 def api(path,body=None,method=None):
  req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'},method=method)
  with urllib.request.urlopen(req,timeout=5) as r:return json.load(r)
 def wait(tid,limit=3):
  deadline=time.monotonic()+limit
  while True:
   task=api('/v1/tasks/'+str(tid))
   if task['status'] not in ('QUEUED','RUNNING','CANCELLING'):return task
   assert time.monotonic()<deadline,('task stuck waiting',task)
   time.sleep(.02)
 def block(key,session=None):
  gates[key]=threading.Event();entered[key]=threading.Event()
  tid=api('/v1/chat',{'message':key,'session':session or key})['id']
  assert entered[key].wait(3),('blocked model did not start',key)
  return tid
 def clear(ids):
  for tid in ids:api('/v1/tasks/'+str(tid),method='DELETE')
  for gate in gates.values():gate.set()
  for tid in ids:wait(tid)
 def send(room):return api('/v1/im/sessions/'+str(room)+'/messages',{'content':'Discuss capacity and finish.','sender':'host'})['task_id']
 with (build/'im-capacity-e2e.log').open('w',encoding='utf-8') as log:
  proc=subprocess.Popen([str(binary),'--state',folder,'--workspace',folder,'serve',str(port)],cwd=root,stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  try:
   for _ in range(100):
    try:api('/v1/scheduler');break
    except OSError:assert proc.poll() is None;time.sleep(.05)
   api('/v1/agents',{'name':'alpha','role':'Architect'})
   room=api('/v1/im/sessions',{'name':'capacity','kind':'group','members':['alpha'],'discussion_rounds':1})['id']
   ids=[block('CAPACITY_BLOCK_'+str(i)) for i in range(4)]
   task_id=send(room);time.sleep(.15)
   assert not group_calls,'a saturated group unexpectedly reached the provider'
   # Lane 0 stays busy. Free lane 1 and require the group to take that lane.
   gates['CAPACITY_BLOCK_1'].set();assert wait(ids[1])['status']=='DONE'
   task=wait(task_id)
   assert task['status']=='DONE' and len(group_calls)==1,task
   assert 'GROUP_CAPACITY_OK' in json.dumps(api('/v1/im/sessions/'+str(room)+'/messages'))
   clear(ids);print('PASS: group starts on free lane 1 while lane 0 remains busy',flush=True)
   ids=[block('CANCEL_BLOCK_'+str(i)) for i in range(4)]
   before=len(group_calls);task_id=send(room);time.sleep(.15)
   api('/v1/tasks/'+str(task_id),method='DELETE');task=wait(task_id)
   assert task['status']=='CANCELLED' and len(group_calls)==before,task
   assert all(api('/v1/tasks/'+str(i))['status']=='RUNNING' for i in ids),'cancelling the waiter affected running chats'
   clear(ids);print('PASS: queued group cancellation completes without releasing busy lanes or calling a model',flush=True)
   first=block('SAME_SESSION_HOLD','same-session')
   second=api('/v1/chat',{'message':'Do not run after cancellation.','session':'same-session'})['id'];time.sleep(.15)
   api('/v1/tasks/'+str(second),method='DELETE');assert wait(second)['status']=='CANCELLED'
   assert api('/v1/tasks/'+str(first))['status']=='RUNNING'
   clear([first]);print('PASS: same-session lock waits cancel without cancelling the owner',flush=True)
   # Historical sessions must not consume lock slots forever and force all
   # later rooms/chats onto session slot zero.
   for i in range(40):assert wait(api('/v1/chat',{'message':'Brief answer.','session':'history-'+str(i)})['id'])['status']=='DONE'
   ids=[block('AFTER_HISTORY_'+str(i)) for i in range(4)]
   clear(ids);assert not errors,errors
   print('PASS: more than 32 historical sessions do not serialize unrelated new chats',flush=True)
  finally:
   for gate in gates.values():gate.set()
   proc.terminate()
   try:proc.wait(timeout=5)
   except subprocess.TimeoutExpired:proc.kill();proc.wait()
   provider.shutdown();provider.server_close()
