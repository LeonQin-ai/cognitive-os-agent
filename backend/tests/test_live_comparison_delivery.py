"""One configured API task: read two synthetic projects and actually deliver HTML.

Explicit protocol selection exercises native or compatibility review; this does not
claim the remote issue reporter's unavailable GLM deployment was reproduced.
"""
import argparse,json,os,socket,subprocess,tempfile,time,urllib.request,uuid
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(__doc__);p.add_argument('--config',required=True,type=Path)
p.add_argument('--report',type=Path,default=root/'build/live-comparison-delivery.json')
p.add_argument('--protocol',choices=('native','legacy'),default='legacy')
p.add_argument('--paged-sources',action='store_true');a=p.parse_args()
raw=json.loads(a.config.read_text(encoding='utf-8-sig'));cfg={k:v for k,v in raw.items() if k.startswith('llm.')}
cfg.update({'http.bind':'127.0.0.1','reasoning.max_rounds':16,'scheduler.workers':1,'llm.timeout_ms':60000})
report={'model':cfg.get('llm.model'),'protocol':a.protocol,'passed':False,'scope':'synthetic code comparison + actual HTML artifact; reporter GLM endpoint/source unavailable'}
a.report.parent.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='coa-live-comparison-') as folder:
 state=Path(folder);left,right=uuid.uuid4().hex[:14],uuid.uuid4().hex[:14]
 (state/'frontend.c').write_text(f'// PROJECT_A_MARKER {left}\nunsigned frontend_mhz(void) {{ return read_sysfs("scaling_cur_freq") / 1000; }}\n',encoding='utf8')
 (state/'hardware.c').write_text(f'// PROJECT_B_MARKER {right}\nconst char *hardware_query = "cat /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_cur_freq"; // values in kHz\n',encoding='utf8')
 if a.paged_sources:
  for name in ('frontend.c','hardware.c'):
   source=state/name
   source.write_text(('// unrelated source commentary for pagination\n'*420)+source.read_text(encoding='utf8'),encoding='utf8')
 report['paged_sources']=a.paged_sources
 (state/'cognitive-os-agent.json').write_text(json.dumps(cfg),encoding='utf8')
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 def api(method,path,body=None):
  req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,method=method,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
  with urllib.request.urlopen(req,timeout=10) as response:return json.load(response)
 with a.report.with_suffix('.log').open('w',encoding='utf8') as log:
  proc=subprocess.Popen([str(root/'build'/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',folder,'--workspace',folder,'serve',str(port)],cwd=root,stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':a.protocol},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  observed={};tid=None
  try:
   for _ in range(150):
    try:api('GET','/v1/scheduler');break
    except OSError:assert proc.poll() is None;time.sleep(.1)
   prompt=('分析当前目录 frontend.c 和 hardware.c 两个虚构测试项目的采样来源、频率单位、差异及优缺点。'
    '实际读取这两份源码，生成 comparison.html 对比报告并写入当前工作区，再实际读取该 HTML 校验。'
    '报告必须包含两个源码中的完整 PROJECT_A_MARKER/PROJECT_B_MARKER 值，以及 scaling_cur_freq、cpuinfo_cur_freq 和单位换算 1000。'
    '最终答复也必须包含两个完整 Marker 值和报告文件名。只使用 list_dir、file_read、file_write，不执行 shell，不修改原始两份代码。'
    '继续完成阅读、分析、写入与校验，不要只交付阶段性计划。')
   started=time.monotonic();tid=api('POST','/v1/tasks',{'prompt':prompt})['id']
   while True:
    observed=api('GET',f'/v1/tasks/{tid}')
    if observed.get('status') not in ('RUNNING','QUEUED'):break
    # Allow the configured 16 work rounds plus bounded finalization. The old
    # 12-call cutoff cancelled valid artifact verification before Runtime's limit.
    if observed.get('model_failures',0)>0 or time.monotonic()-started>180 or observed.get('llm_calls',0)>18:
     report['stop_observation']=observed
     api('DELETE',f'/v1/tasks/{tid}')
     for _ in range(50):
      observed=api('GET',f'/v1/tasks/{tid}')
      if observed.get('status') not in ('RUNNING','QUEUED'):break
      time.sleep(.1)
     raise RuntimeError('model failure or bounded task budget reached; stopped without automatic rerun')
    time.sleep(.2)
   report['seconds']=round(time.monotonic()-started,3)
   report['task']=observed;report['usage']=api('GET','/v1/usage')
   assert observed.get('status')=='DONE' and not observed.get('model_failures'),'task failed or required model retry'
   artifact=state/'comparison.html';html=artifact.read_text(encoding='utf-8-sig') if artifact.exists() else ''
   report['artifact_bytes']=artifact.stat().st_size if artifact.exists() else 0
   report['artifact_passed']=all(x in html for x in (left,right,'scaling_cur_freq','cpuinfo_cur_freq','1000')) and '<html' in html.lower()
   assert report['artifact_passed'],'actual HTML artifact missing required source proofs/comparison'
   assert all(x in observed.get('output','') for x in (left,right,'comparison.html')),'final answer did not deliver verified result'
   steps=observed.get('steps',[]);writes=[i for i,s in enumerate(steps) if s.get('tool')=='file_write' and 'comparison.html' in s.get('args','') and s.get('ok')]
   assert writes and any(i>writes[-1] and s.get('tool')=='file_read' and 'comparison.html' in s.get('args','') and s.get('ok') for i,s in enumerate(steps)),'artifact was not read back after write'
   assert all(s.get('tool') in ('list_dir','file_read','file_write') for s in steps)
   assert left in (state/'frontend.c').read_text() and right in (state/'hardware.c').read_text()
   assert observed.get('tool_protocol')==a.protocol;report['passed']=True
  except Exception as error:
   report['error']=str(error)
  finally:
   artifact=state/'comparison.html'
   if artifact.exists():
    html=artifact.read_text(encoding='utf-8-sig')
    report['artifact_bytes']=artifact.stat().st_size
    report['artifact_passed']=all(x in html for x in (left,right,'scaling_cur_freq','cpuinfo_cur_freq','1000')) and '<html' in html.lower()
    a.report.with_suffix('.html').write_text(html,encoding='utf8')
   try:
    if tid is not None:report['task']=api('GET',f'/v1/tasks/{tid}')
    report['usage']=api('GET','/v1/usage')
   except OSError:pass
   proc.terminate();proc.wait(timeout=10)
   a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps({**{k:v for k,v in report.items() if k not in ('task','stop_observation')},'task':{k:report.get('task',{}).get(k) for k in ('status','tool_protocol','llm_calls','tool_calls','model_failures','tokens_in','tokens_out','usage_available','model_failure')}},ensure_ascii=False))
raise SystemExit(0 if report['passed'] else 1)
