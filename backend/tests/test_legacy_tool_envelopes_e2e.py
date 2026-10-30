"""Fixed wire responses exercise parsing/execution, independent of prompt text.

trace_* reconstruct the Oct 29 17:31 displayed tool sequence. The two prose
responses are synthetic: the report does not include raw provider responses.
Passing them does not reproduce or resolve the deployed GLM failure.
"""
import json, os, socket, subprocess, tempfile, threading, time, urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from fixture_http_response import respond_json
ROOT=Path(__file__).resolve().parents[1];BUILD=ROOT/os.environ.get('COA_BUILD_DIR','build')
BINARY=BUILD/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
HTML='<html>LEFT_WIRE_PROOF RIGHT_WIRE_PROOF</html>'
FINAL='comparison.html 已生成并读回；LEFT_WIRE_PROOF 与 RIGHT_WIRE_PROOF 已核对。'
def function(name,args):return {'type':'function','function':{'name':name,'arguments':json.dumps(args)}}
def exercise(mode,protocol='legacy'):
 calls=0
 expected_html=HTML+('<!--'+('large receipt proof '*6000)+'-->' if mode=='large_html' else '')
 if mode in ('generic_recovery','generic_delayed','generic_stuck','generic_blocked','generic_progress','generic_cached','generic_task110','chat_plain'):
  finish={'tool':'complete_task','args':{'status':'blocked' if mode=='generic_blocked' else 'complete','requires_tools':False,'answer':'没有可用证据，无法继续核对。' if mode=='generic_blocked' else '证据核对完成：LEFT_WIRE_PROOF、RIGHT_WIRE_PROOF。'}}
  replies=['<tool_call>file_read{invalid arguments</tool_call>']
  if mode=='chat_plain':replies=['你好。']
  elif mode=='generic_blocked':replies += [finish]
  elif mode in ('generic_progress','generic_cached'):
   replies += ['准备检查。',{'tool':'file_read','args':{'path':'left.c'}},'需要继续推进。','正在核对。',{'tool':'file_read','args':{'path':'left.c' if mode=='generic_cached' else 'right.c'}},finish]
   if mode=='generic_cached':replies[-1]='换一种说法，继续检查。'
  elif mode=='generic_task110':
   replies=[[{'tool':'list_dir','args':{'path':side}} for side in ('left','right')],'我需要实际读取两个项目的关键文件来进行有意义的对比。让我先并行读取。',[{'tool':'file_read','args':{'path':'left.c'}},{'tool':'file_read','args':{'path':'right.c'}}],
    '我需要继续读取两个项目的更多内容来完成对比分析。让我并行读取两个README的后续部分，并深入查看源码结构。',
    '我需要继续读取两个项目的更多内容来完成对比分析。让我并行读取两个README的后续部分，并查看核心源码结构。',
    '我已经有了两个项目的基础信息。让我继续读取两个README的后续部分以获取功能特性详情，同时查看文档目录结构。',
    '我需要继续读取两个项目的README后续部分和核心源码结构，以完成有意义的对比分析。让我并行执行多个读取操作。']
  else:
   replies += (['正在检查，请稍候。'] if mode=='generic_recovery' else ['正在检查，请稍候。','计划先核对证据。','下一步读取两个文件。'])
   if mode in ('generic_recovery','generic_delayed'):replies += [[{'tool':'file_read','args':{'path':'left.c'}},{'tool':'file_read','args':{'path':'right.c'}}],finish]
 elif mode in ('trace_resume','trace_stuck','trace_after_error','trace_after_error_progress','trace_107','trace_narration_recovery'):
  narration='我需要进一步了解两个项目的源码结构，以便做出详细的对比分析。让我查看两个项目的源码目录结构和关键文件。'
  replies=[[{'tool':'list_dir','args':{'path':side}} for side in ('left','right')],
   [{'tool':'file_read','args':{'path':'left/README.md'}}],
   [{'tool':'file_read','args':{'path':path}} for path in ('left/README.md','right/README.md','left/Cargo.toml','right/Cargo.toml','left/CHARTER.md','left/SPECIFICATION.md')],
   narration,narration]
  if mode=='trace_after_error':replies=[replies[0],'<tool_call>file_read{invalid arguments</tool_call>',narration,narration,narration]
  if mode=='trace_after_error_progress':replies=replies[:3]+['<tool_call>file_read{invalid arguments</tool_call>',{'tool':'file_read','args':{'path':'left.c'}},{'tool':'file_read','args':{'path':'right.c'}},narration,narration]
  if mode=='trace_107':
   replies=[replies[0],'<tool_call>file_read{invalid arguments</tool_call>','<tool_call>file_read{invalid arguments</tool_call>',{'tool':'file_read','args':{'path':'left.c'}},narration,narration,narration,{'tool':'file_read','args':{'path':'right.c'}},narration]
  if mode!='trace_stuck':replies += [[{'tool':'file_read','args':{'path':'left.c'}},{'tool':'file_read','args':{'path':'right.c'}}],{'tool':'file_write','args':{'path':'comparison.html','content':HTML}},{'tool':'file_read','args':{'path':'comparison.html'}},{'tool':'complete_task','args':{'status':'complete','requires_tools':False,'answer':FINAL}}]
  if mode=='trace_narration_recovery':
   reads=[{'tool':'file_read','args':{'path':'left.c'}},{'tool':'file_read','args':{'path':'right.c'}}]
   replies=[reads,narration,reads,narration,narration,narration,{'tool':'file_write','args':{'path':'comparison.html','content':HTML}},{'tool':'file_read','args':{'path':'comparison.html'}},{'tool':'complete_task','args':{'status':'complete','requires_tools':False,'answer':FINAL}}]
  if mode=='trace_107':del replies[9] # Required sources have already been read.
  if mode=='trace_after_error_progress':del replies[8] # Both source reads already executed after the error.
 elif mode in ('envelopes','recover_schema','write_only','stale_read','large_html'):
  replies=[{'tool_calls':[function('file_read',{'path':'left.c'}),function('file_read',{'path':'right.c'})]},function('file_write',{'path':'comparison.html','content':expected_html}),{'tool':'file_read','arguments':{'path':'comparison.html'}},{'name':'complete_task','arguments':{'status':'complete','requires_tools':False,'answer':FINAL}}]
  if mode=='write_only':del replies[2]
  if mode=='stale_read':replies.insert(3,function('file_write',{'path':'comparison.html','content':HTML+'changed'}))
  if mode=='recover_schema':replies.insert(0,[{'tool':'file_write','args':{'path':'must-not-exist.txt','content':'rejected prefix'}},{'tool':'file_read','args':{}}])
 elif mode=='unrelated_schema':replies=[[{'tool':'grep','args':{'path':'required'}}],{'tool':'file_read','args':{'path':'left.c'}},{'tool':'complete_task','args':{'status':'complete','requires_tools':False,'answer':'源码分析完成。'}}]
 elif mode in ('false_done','preexisting'):replies=[{'tool_calls':[function('file_read',{'path':'left.c'}),function('file_read',{'path':'right.c'})]},{'tool':'complete_task','args':{'status':'complete','requires_tools':False,'answer':FINAL}}]
 elif mode=='completion':replies=[{'tool':'complete_task','args':{'status':'complete','requires_tools':False,'answer':'你好。'}}]
 elif mode=='commentary':replies=['我将开始探索这两个项目的结构，以便进行详细的对比。让我先列出这两个目录。']
 else:
  bad={'missing_name':{'args':{'path':'left.c'}},'argument_type':{'tool':'file_read','args':[]},'argument_suffix':{'tool':'file_read','args':'{"path":"left.c"} trailing'},'schema':{'tool':'file_read','args':{}}}[mode]
  replies=[[{'tool':'file_write','args':{'path':'must-not-exist.txt','content':'unsafe partial batch'}},bad]]
 class Provider(BaseHTTPRequestHandler):
  def log_message(self,*_):pass
  def do_POST(self):
   nonlocal calls
   body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
   if mode=='generic_recovery' and protocol=='legacy' and calls==1:
    context=json.dumps(body,ensure_ascii=False)
    assert 'no tool from that response ran' in context, 'planner rejection must not become a fictitious tool failure'
    assert '上一轮有动作执行失败' not in context, 'wrong tool-failure recovery branch'
   # Responses remain fixed and independent of the prompt/recovery hint.
   reply=replies[min(calls,len(replies)-1)];calls+=1
   content=reply if isinstance(reply,str) else json.dumps(reply,ensure_ascii=False)
   message={'content':content}
   if protocol=='native' and not isinstance(reply,str):
    entries=reply if isinstance(reply,list) else reply.get('tool_calls',[reply]);native=[]
    for i,entry in enumerate(entries):
     call=entry if 'function' in entry else function(entry.get('tool',entry.get('name','')),entry.get('args',entry.get('arguments',{})))
     native.append({**call,'id':f'call-{calls}-{i}'})
    message={'content':'','tool_calls':native}
   respond_json(self,json.dumps({'choices':[{'message':message,'finish_reason':'tool_calls' if protocol=='native' else 'stop'}]}).encode())
 server=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=server.serve_forever,daemon=True).start()
 try:
  with tempfile.TemporaryDirectory(prefix='coa-wire-actions-') as folder:
   work=Path(folder);(work/'left.c').write_text('LEFT_WIRE_PROOF',encoding='utf8');(work/'right.c').write_text('RIGHT_WIRE_PROOF',encoding='utf8')
   if mode.startswith('trace_') or mode=='generic_task110':
    for side in ('left','right'):
     (work/side).mkdir()
     for name in ('README.md','Cargo.toml','CHARTER.md','SPECIFICATION.md'):(work/side/name).write_text(side+' '+name,encoding='utf8')
   if mode=='preexisting':(work/'comparison.html').write_text(HTML,encoding='utf8')
   (work/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixed-wire','llm.api_key':'fixture','llm.base_url':f'http://127.0.0.1:{server.server_port}','reasoning.max_rounds':16}),encoding='utf8')
   with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
   def api(path,body=None):
    req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=5) as response:return json.load(response)
   with (BUILD/f'wire-envelopes-{mode}.log').open('w',encoding='utf8') as log:
    proc=subprocess.Popen([str(BINARY),'--state',folder,'--workspace',folder,'serve',str(port)],cwd=ROOT,stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':protocol},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    try:
     deadline=time.monotonic()+25
     for _ in range(150):
      try:api('/v1/scheduler');break
      except OSError:assert proc.poll() is None;time.sleep(.05)
     tid=api('/v1/chat',{'message':'你好' if mode in ('completion','chat_plain') else '核对 left.c 和 right.c 的证据并给出结论。' if mode.startswith('generic_') else '分析 required 目录的代码。' if mode=='unrelated_schema' else '对比一下 left 和 right 两个项目的代码，详细解释差异与优缺点，最后也输出一个html来对比说明。','session':'wire'})['id']
     while True:
      task=api(f'/v1/tasks/{tid}')
      if task['status'] not in ('RUNNING','QUEUED'):break
      assert time.monotonic()<deadline,task;time.sleep(.02)
     assert calls<=(13 if mode=='trace_107' else 12 if mode=='trace_after_error_progress' else 10),(calls,task)
     assert not (work/'must-not-exist.txt').exists(),('invalid batch executed a prefix',mode,task)
     history=api('/v1/chat/history?session=wire')['turns']
     if mode=='generic_recovery':
      assert task['status']=='DONE' and calls==5 and task['model_failures']==2 and task['tool_calls']==2,task
      assert [s['tool'] for s in task['steps']]==['file_read','file_read'],task
      assert task['output']==finish['args']['answer'] and history[-1]['a']==task['output'],task
      assert not (work/'comparison.html').exists()
     elif mode in ('generic_stuck','generic_delayed','generic_blocked','generic_cached','generic_task110'):
      expected_calls={'generic_blocked':2,'generic_cached':7,'generic_task110':6}.get(mode,3)
      expected_tools={'generic_cached':1,'generic_task110':4}.get(mode,0)
      assert task['status']=='FAILED' and calls==expected_calls and task['tool_calls']==expected_tools and not history,task
      if mode!='generic_blocked':
       assert '恢复阶段连续两次' in task['output'],task
       decisions=[json.loads(s['result']) for s in api(f'/v1/trace?task_id={tid}') if s['name']=='llm.plan']
       assert decisions[-1]['no_action_response'],decisions[-1]
     elif mode=='generic_progress':
      assert task['status']=='DONE' and calls==8 and task['tool_calls']==2 and task['model_failures']==3,task
      assert task['output']==finish['args']['answer'],task
     elif mode=='chat_plain':
      assert task['status']=='DONE' and calls==1 and task['tool_calls']==0 and task['output']=='你好。',task
     elif mode in ('trace_after_error','trace_107','trace_narration_recovery'):
      expected={'trace_after_error':(4,2),'trace_107':(7,3),'trace_narration_recovery':(6,2)}[mode]
      assert task['status']=='FAILED' and (calls,task['tool_calls'])==expected and not history,task
      assert '恢复阶段连续两次' in task['output'] and not (work/'comparison.html').exists(),task
     elif mode in ('envelopes','recover_schema','large_html','trace_resume','trace_after_error_progress'):
      if mode=='recover_schema':assert task['model_failures']==1,task
      assert task['status']=='DONE' and task['output']==FINAL,task
      assert (work/'comparison.html').read_text(encoding='utf8')==expected_html
      steps=task['steps']
      if mode=='trace_after_error_progress':
       assert calls==12 and task['model_failures']==2,task
       assert [s['tool'] for s in steps[-4:]]==['file_read','file_read','file_write','file_read'],steps
       steps=steps[-4:]
      elif mode=='trace_resume':
       assert calls==10 and [s['tool'] for s in steps[-4:]]==['file_read','file_read','file_write','file_read'],task
       steps=steps[-4:]
      else:assert task['tool_calls']==4,task
      assert [s['tool'] for s in steps]==['file_read','file_read','file_write','file_read'],steps
      assert 'LEFT_WIRE_PROOF' in steps[0]['out'] and 'RIGHT_WIRE_PROOF' in steps[1]['out'] and HTML in steps[3]['out'],steps
      assert history[-1]['a']==FINAL,history
     elif mode=='trace_stuck':
      assert calls==6 and task['status']=='FAILED' and task['tool_calls']==8 and '产物验收' in task['output'] and not history,task
     elif mode=='completion':
      assert task['status']=='DONE' and task['output']=='你好。' and task['tool_calls']==0 and calls==1,task
     elif mode in ('false_done','preexisting','write_only','stale_read'):
      assert task['status']=='FAILED' and '产物验收' in task['output'] and not history,task
      assert task['tool_calls']==({'write_only':3,'stale_read':5}.get(mode,2)),task
     elif mode=='unrelated_schema':
      assert task['status']=='FAILED' and 'grep' in task['output'] and task['tool_calls']==1 and not history,task
     else:
      assert task['status']=='FAILED' and task['tool_calls']==0 and not history,task
      assert not (work/'comparison.html').exists()
     if mode.startswith('trace_'):
      def check_decisions(spans):
       plans=[s for s in spans if s['name']=='llm.plan']
       assert len(plans)==calls,plans
       decisions=[json.loads(s['result']) for s in plans]
       assert all(s['command']=='planner decision' for s in plans),plans
       assert [d['round'] for d in decisions]==list(range(1,calls+1)),decisions
       assert all(d['max_rounds']==16 and d['tool_protocol']=='legacy' for d in decisions),decisions
       if mode=='trace_narration_recovery':
        assert not decisions[3]['completion_review_required'] and decisions[4]['completion_review_required'],decisions
        assert decisions[5]['completion_review_required'] and decisions[5]['no_action_response'],decisions
        return
       if mode=='trace_107':
        assert [d['planned_actions'] for d in decisions]==[2,0,0,1,0,0,0],decisions
        assert decisions[5]['completion_review_required'] and decisions[6]['completion_review_required'],decisions
        assert decisions[-1]['no_action_response'],decisions[-1]
        return
       if mode=='trace_after_error_progress':
        assert 'invalid or incomplete tagged tool-call response' in decisions[3]['error'],decisions[3]
        assert decisions[4]['completion_review_required'],decisions
        assert not decisions[5]['completion_review_required'] and not decisions[6]['completion_review_required'],decisions
        assert [d['planned_actions'] for d in decisions[4:6]]==[1,1],decisions[4:6]
        assert decisions[6]['parsed_response']==narration and 'error' not in decisions[6],decisions[6]
        assert decisions[7]['completion_review_required'] and 'error' in decisions[7],decisions[7]
        assert decisions[-1]['completion_declaration']=='complete',decisions[-1]
        return
       if mode=='trace_after_error':
        assert decisions[1]['error']=='invalid or incomplete tagged tool-call response; no actions executed; use the configured tool protocol with a valid identifier and complete JSON object',decisions[1]
        assert decisions[2]['completion_review_required'],decisions[2]
        assert all(d['completion_review_required'] and d['no_action_response'] for d in decisions[2:4]),decisions[2:4]
        assert task['model_failures']==3,task
        return
       assert decisions[3]['parsed_response']==narration and decisions[3]['completion_declaration']=='none',decisions[3]
       assert decisions[4]['completion_review_required'] and 'error' in decisions[4] and plans[4]['status']==-1,decisions[4]
       if mode=='trace_resume':assert decisions[-1]['completion_declaration']=='complete',decisions[-1]
      check_decisions(api(f'/v1/trace?task_id={tid}'))
      while True:
       saved=api(f'/v1/trace/history?task_id={tid}')
       if saved:break
       assert time.monotonic()<deadline;time.sleep(.02)
      check_decisions(saved[0]['spans'])
     print('PASS fixed wire:',protocol,mode,task['status'],'calls',calls,'tools',task['tool_calls'],flush=True)
    finally:proc.terminate();proc.wait(timeout=5)
 finally:server.shutdown();server.server_close()
if __name__=='__main__':
 for mode in ('generic_recovery','generic_delayed','generic_stuck','generic_blocked','generic_progress','generic_cached','generic_task110','chat_plain'):exercise(mode)
 for mode in ('trace_narration_recovery','trace_107','trace_after_error_progress','trace_after_error','trace_resume','trace_stuck','false_done','preexisting','write_only','stale_read','envelopes','large_html','recover_schema','unrelated_schema','completion','missing_name','argument_type','argument_suffix','schema','commentary'):exercise(mode)

if __name__=='__main__':
 for mode in ('false_done','preexisting','envelopes','large_html'):exercise(mode,'native')
