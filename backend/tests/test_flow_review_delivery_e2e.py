"""Three real Flow nodes: deliver architecture, preserve a long report, finish review.

Synthetic files reproduce the shape of the user's trace, without publishing
private source or the pasted report. No real provider credentials are used.
"""
import json,os,re,socket,subprocess,tempfile,threading,time,urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from fixture_http_response import respond_json
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
binary=build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
INTRO='我已经收集了足够的架构信息。现在读取保存的 web 包目录列表以补全最后的模块结构，然后撰写架构分析文档。'
NARRATION='我需要阅读尚未完全审查的关键实现文件。让我批量读取基础设施层（文件 I/O 和并发安全最关键）、治理应用服务和上下文压缩服务的核心实现。'
ARCH='架构分析：ARCH_LAYER_BOUNDARY_PROOF 验证 domain 定义接口，application 组织用例，infrastructure 实现文件存储与互斥锁。'
REPORT='记忆与上下文实现分析：MEMORY_ENTRY_PROOF 和 CONTEXT_COMPACT_PROOF 已从源码核对。\n'+''.join(f'设计核对 {i:04d}: 接口编号 {i*17+1}；不变量 {i*i+11}；存储路径经调用方注入。\n' for i in range(120))+'\nUNREAD_CRITICAL_FILES: infra/store.py, infra/locking.py\n这些基础设施文件尚未完成审查，必须按实际源码验证原子写入和并发互斥。REPORT_TAIL_END'
REVIEW='审查结论：STORE_ATOMIC_PROOF 验证临时文件替换；LOCK_MUTEX_PROOF 验证写入互斥。剩余风险：未执行跨进程压力测试。'
assert len(REPORT.encode())>4096

def exercise(mode,protocol):
 counts={};requests=[];errors=[];offset=0;report_path=None;source_read=False
 pending=threading.Event();release=threading.Event();announced_offsets=set()
 with tempfile.TemporaryDirectory(prefix='coa-flow-delivery-') as folder:
  work=Path(folder);state=work/'state';state.mkdir();(work/'infra').mkdir()
  (work/'nested/src/context').mkdir(parents=True)
  (work/'nested/src/context/proof.py').write_text('CONTEXT_DIRECTORY_PROOF',encoding='utf-8')
  for i in range(7):(work/f'scan{i}').mkdir();(work/f'scan{i}'/f'entry{i}.txt').write_text(str(i))
  for name,proof in [('core.py','ARCH_LAYER_BOUNDARY_PROOF'),('memory.py','MEMORY_ENTRY_PROOF'),('context.py','CONTEXT_COMPACT_PROOF'),('infra/store.py','STORE_ATOMIC_PROOF'),('infra/locking.py','LOCK_MUTEX_PROOF')]:
   (work/name).write_text(proof+'\n',encoding='utf-8')
  class Provider(BaseHTTPRequestHandler):
   def log_message(self,*_):pass
   def do_POST(self):
    nonlocal offset,report_path,source_read
    try:
     body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
     text='\n'.join(m['content'] for m in body['messages'] if isinstance(m.get('content'),str))
     current=text.rsplit('## Current request\n',1)[-1].split('\n## Current execution state',1)[0]
     role='IM' if ' in an IM group' in current else next(k for k in ('ARCHITECT','CODER','REVIEWER') if 'ROLE_'+k in current)
     n=counts.get(role,0);counts[role]=n+1;requests.append((role,body))
     review='COMPLETION REVIEW PROTOCOL:' in body['messages'][0]['content'] or (protocol=='native' and 'Completion review:' in text)
     def finish(answer):return json.dumps([{'tool':'complete_task','args':{'status':'complete','requires_tools':False,'answer':answer}}]) if review or protocol=='native' else answer
     if mode=='cancel' and role=='ARCHITECT' and n==0:
      # Preparation intentionally exceeds the removed 5s readiness timeout.
      time.sleep(float(os.environ.get('COA_FIXTURE_SETUP_DELAY_MS','5500'))/1000)
     if mode=='cancel' and role=='REVIEWER' and n==8:pending.set();release.wait(20)
     if role=='IM':answer=finish('IM_CONCURRENT_OK [[DISCUSSION_DONE]]')
     elif role=='ARCHITECT':
      if n==0:answer=json.dumps([{'tool':'list_dir','args':{'path':'.'}},{'tool':'file_read','args':{'path':'core.py'}}])
      elif n==1:answer=finish(INTRO)
      elif n==2:
       assert review,'architecture announcement was accepted without delivery review'
       assert 'ARCH_LAYER_BOUNDARY_PROOF' in text,'architecture source was not observed'
       answer=json.dumps([{'tool':'file_write','args':{'path':'architecture.md','content':ARCH}},{'tool':'file_read','args':{'path':'architecture.md'}}])
      else:
       assert ARCH in text;answer=finish(ARCH)
     elif role=='CODER':
      assert ARCH in current and INTRO not in current,'upstream progress was forwarded as architecture'
      if n==0:answer=json.dumps([{'tool':'file_read','args':{'path':p}} for p in ('memory.py','context.py')])
      else:
       assert 'MEMORY_ENTRY_PROOF' in text and 'CONTEXT_COMPACT_PROOF' in text
       answer=finish(REPORT)
     elif mode=='directory' and n==0:answer=json.dumps([{'tool':'list_dir','args':{'path':'src/context'}}])
     elif mode=='directory' and n==1:answer=json.dumps([{'tool':'list_dir','args':{'path':'nested/src/context'}},{'tool':'file_read','args':{'path':'nested/src/context/proof.py'}}])
     elif n<7:answer=json.dumps([{'tool':'list_dir','args':{'path':f'scan{n}'}}])
     elif n==7 or not review or mode=='stuck':answer=finish(NARRATION)
     elif not source_read:
      assert current.rstrip().endswith('审查其正确性、边界条件、资源泄漏和并发安全，指出潜在风险并提供修复建议。'),'downstream instruction was truncated'
      match=re.search(r'Full result, original instruction and execution workspace: (.*?)\. Read this file',current)
      assert match,'long upstream report was silently truncated without a full-result reference'
      report_path=match.group(1)
      assert Path(report_path).is_file()
      if mode=='repeated_progress' and 0<offset<=8000 and offset not in announced_offsets:
       announced_offsets.add(offset);answer=finish(NARRATION)
      elif 'REPORT_TAIL_END' not in text:
       answer=json.dumps([{'tool':'file_read','args':{'path':report_path,'offset_bytes':offset,'max_bytes':4000}}]);offset+=4000
      else:
       assert 'UNREAD_CRITICAL_FILES: infra/store.py, infra/locking.py' in text
       source_read=True;answer=json.dumps([{'tool':'file_read','args':{'path':p}} for p in ('infra/store.py','infra/locking.py')])
     else:
      assert 'STORE_ATOMIC_PROOF' in text and 'LOCK_MUTEX_PROOF' in text
      answer=finish(REVIEW)
     if protocol=='native':
      actions=json.loads(answer)
      if mode=='stuck' and role=='REVIEWER' and n>=8:
       # Native failure is a structural contradiction: work still requires
       # tools. Never require the runtime to classify prose keywords.
       for action in actions:
        if action['tool']=='complete_task':action['args']['requires_tools']=True
      message={'content':'','tool_calls':[{'id':f'c-{role}-{n}-{i}','type':'function','function':{'name':action['tool'],'arguments':json.dumps(action['args'])}} for i,action in enumerate(actions)]}
     else:message={'content':answer}
     raw=json.dumps({'choices':[{'message':message,'finish_reason':'tool_calls' if protocol=='native' else 'stop'}]}).encode()
     respond_json(self,raw)
    except Exception as error:errors.append(repr(error));raise
  provider=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=provider.serve_forever,daemon=True).start()
  (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'flow-review-fixture','llm.api_key':'fixture','llm.base_url':f'http://127.0.0.1:{provider.server_port}','reasoning.max_rounds':40}),encoding='utf-8')
  with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
  def api(path,body=None):
   req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
   with urllib.request.urlopen(req,timeout=5) as r:return json.load(r)
  with (build/f'flow-review-delivery-{protocol}-{mode}.log').open('w',encoding='utf-8') as log:
   proc=subprocess.Popen([str(binary),'--state',str(state),'--workspace',str(work),'serve',str(port)],cwd=root,stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':protocol},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
   try:
    for _ in range(100):
     try:api('/v1/scheduler');break
     except OSError:assert proc.poll() is None;time.sleep(.05)
    for role in ('ARCHITECT','CODER','REVIEWER'):api('/v1/agents',{'name':role.lower(),'role':'ROLE_'+role})
    dag={'nodes':[{'id':'step1','agent':'architect','task':'分析整体代码架构，梳理模块边界和数据流，输出架构分析文档。'},
     {'id':'step2','agent':'coder','task':'基于 {{step1}} 分析记忆与上下文实现，给出分析报告和尚未审查的文件。'},
     {'id':'step3','agent':'reviewer','task':'基于 {{step2}} 的记忆与上下文实现代码，审查其正确性、边界条件、资源泄漏和并发安全，指出潜在风险并提供修复建议。'}]}
    if mode=='wide':dag['nodes'][2]['task']='基于 {{step2}} 和补充对照 {{step2}} 的记忆与上下文实现代码，审查其正确性、边界条件、资源泄漏和并发安全，指出潜在风险并提供修复建议。'
    tid=api('/v1/flows',dag)['id'];deadline=time.monotonic()+25
    if mode=='cancel':
     # Preparing two upstream nodes and entering review has the same budget
     # as other Flow cases. Only the actual cancellation is timed at 3s.
     while not pending.wait(.1):
      preparation=api('/v1/tasks/'+str(tid))
      diagnostics={'status':preparation['status'],'output':(preparation.get('output') or '')[-1200:],
       'nodes':[{k:n.get(k) for k in ('id','status','stage','model_error','llm_calls','tool_calls')} for n in preparation.get('flow_nodes',[])],
       'fixture_errors':errors}
      assert preparation['status'] in ('QUEUED','RUNNING'),('Flow ended before silent reviewer request',diagnostics)
      assert time.monotonic()<deadline,('Flow preparation did not reach silent reviewer request within its budget',diagnostics)
     room=api('/v1/im/sessions',{'name':'concurrent','kind':'group','members':['architect'],'discussion_rounds':1})['id']
     group=api('/v1/im/sessions/'+str(room)+'/messages',{'content':'Discuss and finish.'})['task_id']
     group_deadline=time.monotonic()+3
     while api('/v1/tasks/'+str(group))['status'] not in ('DONE','FAILED'):
      assert time.monotonic()<group_deadline;time.sleep(.02)
     assert api('/v1/tasks/'+str(group))['status']=='DONE'
     cancel_started=time.monotonic()
     req=urllib.request.Request(f'http://127.0.0.1:{port}/v1/tasks/{tid}',method='DELETE')
     with urllib.request.urlopen(req,timeout=5) as response:json.load(response)
     deadline=cancel_started+3
    while True:
     task=api('/v1/tasks/'+str(tid))
     if task['status'] not in ('QUEUED','RUNNING'):break
     assert time.monotonic()<deadline,task;time.sleep(.02)
    assert not errors,errors
    nodes={node['id']:node for node in task['flow_nodes']}
    assert nodes['step1']['status']==nodes['step2']['status']=='ok',task
    assert (work/'architecture.md').read_text(encoding='utf-8')==ARCH
    assert counts['ARCHITECT']==(5 if protocol=='native' else 4) and counts['CODER']==3,counts
    if mode in ('recover','wide','directory','repeated_progress'):
     assert task['status']=='DONE' and nodes['step3']['status']=='ok' and REVIEW in task['output'],task
     reads=[s for s in nodes['step3']['steps'] if s['tool']=='file_read']
     assert any('REPORT_TAIL_END' in s['out'] for s in reads),reads
     assert sum(s['target'] in ('infra/store.py','infra/locking.py') for s in reads)==2,reads
     assert report_path and REPORT in Path(report_path).read_text(encoding='utf-8')
     assert counts['REVIEWER']<=25,counts
     if mode=='repeated_progress':
      assert announced_offsets=={4000,8000},announced_offsets
     if mode=='directory':
      steps=nodes['step3']['steps']
      assert any(s['tool']=='list_dir' and not s['ok'] for s in steps),steps
      assert any(s['tool']=='file_read' and 'CONTEXT_DIRECTORY_PROOF' in s['out'] for s in steps),steps
    elif mode=='cancel':
     assert time.monotonic()-cancel_started<3,('Flow cancellation exceeded 3 seconds',task)
     assert task['status']=='CANCELLED' and nodes['step3']['status']=='cancelled',task
     assert not source_read and counts.get('IM')==1
    else:
     assert task['status']=='FAILED' and nodes['step3']['status']=='error',task
     assert not source_read and REVIEW not in task['output']
     assert counts['REVIEWER']<=14,counts
    print('PASS:',protocol,mode,task['status'],counts,'architecture artifact and actual reviewer evidence validated',flush=True)
   finally:
    release.set();proc.terminate();proc.wait(timeout=5);provider.shutdown();provider.server_close()
for protocol in ('legacy','native'):
 for mode in ('recover','wide','stuck','cancel','directory','repeated_progress'):exercise(mode,protocol)
