"""Durable session creation/paging through Agent REST; optional paid native regression."""
import argparse,json,os,socket,subprocess,tempfile,time,urllib.request,threading,re
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--state',type=Path);p.add_argument('--config',type=Path);a=p.parse_args()
backend=Path(__file__).resolve().parents[1];build=backend/os.environ.get('COA_BUILD_DIR','build')
with tempfile.TemporaryDirectory(prefix='coa-session-paging-') as tmp:
 state=a.state.resolve() if a.state else Path(tmp);state.mkdir(parents=True,exist_ok=True)
 if not a.state:
  legacy=state/'chat';legacy.mkdir()
  rows=[{'id':f'legacy-{i:08d}','title':'legacy migration title '*4,'created_ms':i+1,'last_active_ms':i+1,'total_ms':0,'shared_memory':True} for i in range(3000)]
  rows[0]['last_active_ms']=4000
  raw=json.dumps(rows);assert len(raw)>512*1024
  (legacy/'sessions.json').write_text(raw,encoding='utf-8')
 config={'llm.provider':'mock','scheduler.workers':1,'reasoning.max_rounds':-1}
 if a.config:
  source=json.loads(a.config.read_text(encoding='utf-8'));config.update({k:v for k,v in source.items() if k.startswith('llm.')})
  assert config['llm.provider']!='mock' and config.get('llm.api_key')
 fixture=None
 if not a.config:
  class Provider(BaseHTTPRequestHandler):
   def log_message(self,*args):pass
   def do_POST(self):
    body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
    markers=re.findall(r'(?:FIRST|SECOND)-[0-9]+',body['messages'][-1]['content'])
    answer=markers[-1] if markers else 'fixture answer'
    reply={'choices':[{'message':{'content':None,'tool_calls':[{'id':'finish','type':'function','function':{'name':'complete_task','arguments':json.dumps({'status':'complete','requires_tools':False,'answer':answer})}}]},'finish_reason':'tool_calls'}]}
    raw=json.dumps(reply).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
  fixture=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=fixture.serve_forever,daemon=True).start()
  config.update({'llm.provider':'openai','llm.model':'local-native-fixture','llm.api_key':'fixture-only','llm.base_url':f'http://127.0.0.1:{fixture.server_port}'})
 config['llm.routing']='fixed'
 (state/'cognitive-os-agent.json').write_text(json.dumps(config),encoding='utf-8')
 with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
 def req(path,body=None):
  data=json.dumps(body).encode() if body is not None else None
  r=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=data,headers={'Content-Type':'application/json'})
  with urllib.request.urlopen(r,timeout=15) as response:return json.load(response)
 report={'state':str(state),'model':config.get('llm.model','mock'),'tasks':[],'completed':False}
 def start():
  log=(state/'paging-server.log').open('a',encoding='utf-8')
  child=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',str(state),'--workspace',str(state),'serve',str(port)],cwd=backend,stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  for _ in range(300):
   try:req('/v1/scheduler');return child,log
   except OSError:assert child.poll() is None;time.sleep(.1)
  raise TimeoutError('startup')
 def stop(child,log):
  child.terminate()
  try:child.wait(timeout=10)
  except subprocess.TimeoutExpired:child.kill();child.wait()
  log.close()
 child,log=start()
 try:
  before=req('/v1/chat/sessions?limit=12')['total']
  if not a.state:
   assert before==3000,'legacy index larger than 512 KiB must migrate without loss'
   assert req('/v1/chat/sessions?limit=12')['sessions'][0]['id']=='legacy-00000000','migration preserves last activity order'
  ids=[req('/v1/chat/sessions',{})['id'] for _ in range(160)]
  assert len(set(ids))==160
  first=req('/v1/chat/sessions?limit=12');assert first['total']==before+160 and len(first['sessions'])==12
  assert first['sessions'][0]['id']==ids[-1]
  cursor='';seen=[]
  # Enumerate exactly the 160 fresh REST-created sessions, independent of the preloaded corpus.
  while len(seen)<160:
   page=req('/v1/chat/sessions?limit=12'+('&cursor='+cursor if cursor else ''))
   seen.extend(s['id'] for s in page['sessions']);cursor=page['next_cursor']
  assert seen[:160]==ids[::-1] and len(set(seen[:160]))==160
  report.update({'before':before,'created_http':160,'enumerated_http':160,'port':port})
  print(json.dumps({'phase':'browser_ready','port':port,'state':str(state)},ensure_ascii=False),flush=True)
  # Browser smoke can attach without issuing model requests.
  node=backend/'tests/test_session_paging_ui.cjs'
  if os.environ.get('COA_PAGING_BROWSER')=='1':
   subprocess.run(['node',str(node)],check=True,env={**os.environ,'UI_TEST_URL':f'http://127.0.0.1:{port}'},timeout=90)
  for i,sid in enumerate((ids[0],ids[-1])):
   marker=('FIRST-' if not i else 'SECOND-')+str(time.time_ns())
   task=req('/v1/chat',{'message':'这是会话隔离回归，请只回复这个标记：'+marker,'session':sid});deadline=time.monotonic()+120
   while True:
    t=req('/v1/tasks/'+str(task['id']))
    report['in_flight']={k:t.get(k) for k in ('status','model_call','tokens_in','tokens_out','usage_available')}
    if t['status'] not in ('QUEUED','RUNNING','CANCELLING'):break
    if time.monotonic()>deadline:raise TimeoutError('in-flight model request; usage/cost unknown')
    time.sleep(.25)
   report.pop('in_flight',None)
   report['tasks'].append({k:t.get(k) for k in ('status','llm_calls','tool_calls','model_failures','tokens_in','tokens_out','usage_available','elapsed_ms')})
   report['tasks'][-1]['session']=sid
   assert t['status']=='DONE' and marker in t.get('output',''),t.get('output','')[:300]
  h1=req('/v1/chat/history?session='+ids[0]);h2=req('/v1/chat/history?session='+ids[-1])
  text1=json.dumps(h1,ensure_ascii=False);text2=json.dumps(h2,ensure_ascii=False)
  assert 'FIRST-' in text1 and 'SECOND-' not in text1 and 'SECOND-' in text2 and 'FIRST-' not in text2
  report['histories_isolated']=True
 finally:
  stop(child,log)
  (state/'paging-report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
 journal=state/'chat/sessions-index.jsonl'
 with journal.open('ab') as f:f.write(b'{"id":"interrupted-tail')
 child,log=start()
 try:
  page=req('/v1/chat/sessions?limit=12');assert page['total']>=before+160
  assert 'FIRST-' in json.dumps(req('/v1/chat/history?session='+ids[0]),ensure_ascii=False)
  after_tail=req('/v1/chat/sessions',{})['id']
  assert after_tail not in ids
  report['restart_verified']=True
 finally:stop(child,log)
 child,log=start()
 try:
  page=req('/v1/chat/sessions?limit=12')
  assert any(s['id']==after_tail for s in page['sessions'])
  report['interrupted_tail_recovered']=True;report['completed']=True
 finally:stop(child,log)
 if fixture:fixture.shutdown();fixture.server_close()
 (state/'paging-report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
 print('PASS: real REST creation, cursor pages, separate histories and restart',json.dumps(report,ensure_ascii=False),flush=True)
