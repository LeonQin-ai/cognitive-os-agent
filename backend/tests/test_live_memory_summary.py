"""One opt-in configured Runtime summary call, then restart/read without inference."""
import argparse,json,os,socket,subprocess,tempfile,time,urllib.request,uuid
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(__doc__);p.add_argument('--config',required=True,type=Path)
p.add_argument('--report',type=Path,default=root/'build/live-memory-summary.json');a=p.parse_args()
cfg=json.loads(a.config.read_text(encoding='utf-8-sig'));cfg.update({'auth.key':'','http.bind':'127.0.0.1'})
report={'model':cfg.get('llm.model'),'passed':False,'scope':'one configured Runtime read-only memory-summary request; no retries'}
binary=root/'build'/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
a.report.parent.mkdir(parents=True,exist_ok=True);proc=None
with tempfile.TemporaryDirectory(prefix='coa-live-summary-') as folder:
 state=Path(folder);work=state/'workspace';work.mkdir();(state/'memory').mkdir()
 project='Project_'+uuid.uuid4().hex[:10];proof=uuid.uuid4().hex[:14]
 facts={'project_name':project,'release_verification_code':proof,'constraint':'Do not modify src/runtime; deliver only a read-only summary. All project data is fictional test data.'}
 (state/'memory/facts.json').write_text(json.dumps(facts),encoding='utf8')
 (state/'cognitive-os-agent.json').write_text(json.dumps(cfg),encoding='utf8')
 def api(method,path,body=None):
  req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,method=method,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
  with urllib.request.urlopen(req,timeout=10) as r:return json.load(r)
 with a.report.with_suffix('.log').open('w',encoding='utf8') as log:
  def start():
   global proc,port
   with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
   proc=subprocess.Popen([str(binary),'--state',str(state),'--workspace',str(work),'serve',str(port)],cwd=root,stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
   for _ in range(150):
    try:api('GET','/v1/scheduler');return
    except OSError:assert proc.poll() is None;time.sleep(.1)
   raise TimeoutError('private Runtime not ready')
  def stop():
   global proc
   if proc:proc.terminate();proc.wait(timeout=10);proc=None
  try:
   start();before=api('GET','/v1/memory');started=time.monotonic()
   tid=api('POST','/v1/memory/summary',{})['task_id'];observed={}
   while True:
    observed=api('GET','/v1/memory/summary');job=observed.get('job',{})
    if job.get('status') not in ('RUNNING','QUEUED'):break
    if time.monotonic()-started>110:
     api('DELETE','/v1/memory/summary');raise TimeoutError('private summary cancelled after 110s')
    time.sleep(.2)
   report['observed']=observed;report['usage']=api('GET','/v1/usage')
   report['seconds']=round(time.monotonic()-started,3)
   summary=observed.get('summary',{})
   assert job.get('status')=='DONE' and summary.get('task_id')==tid, 'summary task failed; retained without retry'
   report['facts_preserved']=all(value in summary.get('text','') for value in (project,proof,'src/runtime'))
   assert report['facts_preserved'],'random project facts/constraint missing from actual generated summary'
   assert summary.get('usage_available') and not summary.get('model_failures'),'usage missing or model failed'
   task=api('GET',f'/v1/tasks/{tid}');report['task']={k:task.get(k) for k in ('status','llm_calls','tool_calls','tokens_in','tokens_out','usage_available')}
   assert task.get('tool_calls')==0 and task.get('llm_calls')==1
   assert api('GET','/v1/memory')==before and not list(work.iterdir()),'summary mutated memory or workspace'
   stop();start();restored=api('GET','/v1/memory/summary').get('summary')
   assert restored==summary,'saved summary changed after restart'
   report['restart_usage']=api('GET','/v1/usage')
   assert not report['restart_usage'].get('usage_available'),'viewing a saved summary unexpectedly invoked the model'
   report['passed']=True
  except Exception as error:
   report['error']=str(error)
   try:report['usage']=api('GET','/v1/usage');report['observed']=api('GET','/v1/memory/summary')
   except OSError:pass
  finally:
   stop();a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps({k:v for k,v in report.items() if k not in ('observed',)},ensure_ascii=False))
raise SystemExit(0 if report['passed'] else 1)
