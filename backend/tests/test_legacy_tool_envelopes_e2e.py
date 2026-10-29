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
 if mode in ('trace_resume','trace_stuck'):
  narration='我需要进一步了解两个项目的源码结构，以便做出详细的对比分析。让我查看两个项目的源码目录结构和关键文件。'
  replies=[[{'tool':'list_dir','args':{'path':side}} for side in ('left','right')],
   [{'tool':'file_read','args':{'path':'left/README.md'}}],
   [{'tool':'file_read','args':{'path':path}} for path in ('left/README.md','right/README.md','left/Cargo.toml','right/Cargo.toml','left/CHARTER.md','left/SPECIFICATION.md')],
   narration,narration]
  if mode=='trace_resume':replies += [[{'tool':'file_read','args':{'path':'left.c'}},{'tool':'file_read','args':{'path':'right.c'}}],{'tool':'file_write','args':{'path':'comparison.html','content':HTML}},{'tool':'file_read','args':{'path':'comparison.html'}},{'tool':'complete_task','args':{'status':'complete','requires_tools':False,'answer':FINAL}}]
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
   self.rfile.read(int(self.headers['Content-Length'])) # Never inspect a prompt or recovery hint.
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
   if mode.startswith('trace_'):
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
     tid=api('/v1/chat',{'message':'你好' if mode=='completion' else '分析 required 目录的代码。' if mode=='unrelated_schema' else '对比一下 left 和 right 两个项目的代码，详细解释差异与优缺点，最后也输出一个html来对比说明。','session':'wire'})['id']
     while True:
      task=api(f'/v1/tasks/{tid}')
      if task['status'] not in ('RUNNING','QUEUED'):break
      assert time.monotonic()<deadline,task;time.sleep(.02)
     assert calls<=10,(calls,task)
     assert not (work/'must-not-exist.txt').exists(),('invalid batch executed a prefix',mode,task)
     history=api('/v1/chat/history?session=wire')['turns']
     if mode in ('envelopes','recover_schema','large_html','trace_resume'):
      if mode=='recover_schema':assert task['model_failures']==1,task
      assert task['status']=='DONE' and task['output']==FINAL,task
      assert (work/'comparison.html').read_text(encoding='utf8')==expected_html
      steps=task['steps']
      if mode=='trace_resume':
       assert calls==10 and [s['tool'] for s in steps[-4:]]==['file_read','file_read','file_write','file_read'],task
       steps=steps[-4:]
      else:assert task['tool_calls']==4,task
      assert [s['tool'] for s in steps]==['file_read','file_read','file_write','file_read'],steps
      assert 'LEFT_WIRE_PROOF' in steps[0]['out'] and 'RIGHT_WIRE_PROOF' in steps[1]['out'] and HTML in steps[3]['out'],steps
      assert history[-1]['a']==FINAL,history
     elif mode=='trace_stuck':
      assert calls==9 and task['status']=='FAILED' and task['tool_calls']==8 and '产物验收' in task['output'] and not history,task
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
     print('PASS fixed wire:',protocol,mode,task['status'],'calls',calls,'tools',task['tool_calls'],flush=True)
    finally:proc.terminate();proc.wait(timeout=5)
 finally:server.shutdown();server.server_close()
if __name__=='__main__':
 for mode in ('trace_resume','trace_stuck','false_done','preexisting','write_only','stale_read','envelopes','large_html','recover_schema','unrelated_schema','completion','missing_name','argument_type','argument_suffix','schema','commentary'):exercise(mode)

if __name__=='__main__':
 for mode in ('false_done','preexisting','envelopes','large_html'):exercise(mode,'native')
