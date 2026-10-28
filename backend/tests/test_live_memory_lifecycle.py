"""Opt-in configured Runtime API benchmark; random facts, new sessions, process restart.

Never prints configuration or credentials. A failed score remains in the report.
"""
import argparse, json, os, shutil, socket, subprocess, tempfile, time, urllib.request, uuid
from pathlib import Path

root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(__doc__)
p.add_argument('--config',required=True,type=Path)
p.add_argument('--repetitions',type=int,default=3)
p.add_argument('--extended',action='store_true',help='also test superseding facts and unknown-fact abstention')
p.add_argument('--abstention-only',action='store_true',help='one bounded diagnostic probe per repetition, without repeating successful memory cases')
p.add_argument('--allow-inefficient',action='store_true',help='report efficiency failures without a nonzero exit (accuracy still required)')
p.add_argument('--report',type=Path,default=root/'build/live-memory-report.json')
a=p.parse_args()
if not 1<=a.repetitions<=10:p.error('repetitions must be 1..10')
config=json.loads(a.config.read_text(encoding='utf-8-sig'))
binary=root/'build'/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
report={'model':config.get('llm.model'),'cases':[],'scope':'natural-language episode persistence/use; shared-memory on/off, not L0/L1/L2 ablation'}
proc=None
with tempfile.TemporaryDirectory(prefix='coa-live-memory-') as directory:
 base=Path(directory);work=base/'work';work.mkdir()
 config.update({'auth.key':'','http.bind':'127.0.0.1','reasoning.max_rounds':5,'llm.timeout_ms':45000})
 def request(method,path,body=None):
  req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,method=method,
   data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
  with urllib.request.urlopen(req,timeout=10) as response:return json.load(response)
 def stop():
  global proc
  if proc is not None:proc.terminate();proc.wait(timeout=10);proc=None
 def start(state):
  global proc,port
  with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
  state.mkdir(exist_ok=True);(state/'cognitive-os-agent.json').write_text(json.dumps(config),encoding='utf8')
  proc=subprocess.Popen([str(binary),'--state',str(state),'--workspace',str(work),'serve',str(port)],
   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  for _ in range(150):
   try:request('GET','/v1/scheduler');return
   except OSError:
    if proc.poll() is not None:raise RuntimeError('private runtime exited')
    time.sleep(.1)
  raise RuntimeError('private runtime not ready')
 def task(label,message,expected,shared=True):
  session=request('POST','/v1/chat/sessions',{})['id']
  request('POST',f'/v1/chat/sessions/{session}/shared',{'shared':shared})
  started=time.monotonic();tid=request('POST','/v1/chat',{'session':session,'message':message,'permission_mode':'ask'})['id']
  result={}
  try:
   while True:
    result=request('GET',f'/v1/tasks/{tid}')
    if result['status'] not in ('RUNNING','QUEUED'):break
    if time.monotonic()-started>90:
     request('DELETE',f'/v1/tasks/{tid}');raise TimeoutError('case exceeded 90s; private task cancelled')
    time.sleep(.2)
   row={k:result.get(k) for k in ('status','llm_calls','tool_calls','tokens_in','tokens_out','model_failures','usage_available','model_failure')}
   row.update(case=label,session=session,seconds=round(time.monotonic()-started,3),
    expected=expected,answer=result.get('output',''),passed=result['status']=='DONE' and result.get('output','').strip()==expected)
   report['cases'].append(row)
   row['efficiency_passed']=row['passed'] and row.get('llm_calls')==1 and not row.get('model_failures') and (row.get('tokens_out') or 0)<=2048
   print(json.dumps({k:v for k,v in row.items() if k not in ('answer','session','expected')},ensure_ascii=False),flush=True)
  except Exception as error:
   row={k:result.get(k) for k in ('status','llm_calls','tool_calls','tokens_in','tokens_out','model_failures','usage_available','model_failure')}
   row.update(case=label,session=session,seconds=round(time.monotonic()-started,3),passed=False,efficiency_passed=False,error=type(error).__name__)
   report['cases'].append(row)
   raise
  finally:
   a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
 try:
  for repetition in range(a.repetitions):
   state=base/f'seed-{repetition}';start(state)
   if a.abstention_only:
    task(f'{repetition}:unknown-diagnostic',f'项目 Unseen{uuid.uuid4().hex[:10]} 的负责人是谁？没有已保存事实时只输出 UNKNOWN，不猜测，不调用工具。','UNKNOWN')
    stop();continue
   project='Project'+uuid.uuid4().hex[:10];value=uuid.uuid4().hex[:16]
   task(f'{repetition}:write',f'请记住项目事实：{project} 的发布校验码是 {value}。这是本测试虚构数据，不是凭据。只回答 RECORDED，不调用工具。','RECORDED')
   stop()
   # Both branches receive identical persisted source, before either query runs.
   for enabled in (False,True):
    branch=base/f'branch-{repetition}-{enabled}';shutil.copytree(state,branch);start(branch)
    task(f'{repetition}:restart-recall:{enabled}',f'根据已保存的历史，{project} 的发布校验码是什么？只输出校验码；如果没有记录，只输出 UNKNOWN。不要猜测，不调用工具。',value if enabled else 'UNKNOWN',enabled)
    if enabled and a.extended:
     revised=uuid.uuid4().hex[:16]
     task(f'{repetition}:update',f'更新项目事实：{project} 的发布校验码已作废，现在唯一有效的新校验码是 {revised}。此前记录仅用于审计，不再有效。这是虚构测试数据。只回答 UPDATED，不调用工具。','UPDATED')
     stop();start(branch)
     task(f'{repetition}:updated-recall',f'根据历史更新，{project} 目前唯一有效的发布校验码是什么？只输出最新有效校验码；如果没有记录，只输出 UNKNOWN。不要猜测，不调用工具。',revised)
     task(f'{repetition}:unknown',f'项目 Unseen{uuid.uuid4().hex[:10]} 的负责人是谁？没有已保存事实时只输出 UNKNOWN，不猜测，不调用工具。','UNKNOWN')
    stop()
  report['passed']=sum(x['passed'] for x in report['cases']);report['total']=len(report['cases'])
  report['tokens_in']=sum(x.get('tokens_in') or 0 for x in report['cases']);report['tokens_out']=sum(x.get('tokens_out') or 0 for x in report['cases'])
  report['efficiency_passed']=sum(x.get('efficiency_passed',False) for x in report['cases'])
  report['limitations']=['No semantic corpus score','No 100-agent authorization claim','Restart occurs after completed seed; not mid-write crash','No 500-turn compaction claim','Recall task benefit only; not code repair benefit']
 finally:
  stop();a.report.parent.mkdir(parents=True,exist_ok=True);a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps({k:v for k,v in report.items() if k!='cases'},ensure_ascii=False))
required=a.repetitions*(1 if a.abstention_only else 6 if a.extended else 3)
raise SystemExit(0 if report.get('passed')==required and (a.allow_inefficient or report.get('efficiency_passed')==required) else 1)
