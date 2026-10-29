"""#232: missing HTML delivery gets a structured recovery contract before abort."""
import json, os, socket, subprocess, tempfile, threading, time, urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from fixture_http_response import respond_json
ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/os.environ.get('COA_BUILD_DIR','build')
BINARY=BUILD/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
PROGRESS='我将并行继续阅读两个 README 的关键部分，并探索源码结构。让我读取两个项目的续读部分。'
FINAL='对比结论：LEFT_SOURCE_PROOF 使用 scaling_cur_freq，RIGHT_SOURCE_PROOF 使用 cpuinfo_cur_freq；comparison.html 已写入并读回验证。'
HTML='<html><body>LEFT_SOURCE_PROOF scaling_cur_freq; RIGHT_SOURCE_PROOF cpuinfo_cur_freq; 1000</body></html>'

def exercise(protocol, mode):
 calls=[];errors=[];phase=0;reviews=0
 class Provider(BaseHTTPRequestHandler):
  def log_message(self,*_):pass
  def do_POST(self):
   nonlocal phase,reviews
   try:
    body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
    text=json.dumps(body,ensure_ascii=False)
    review='COMPLETION REVIEW PROTOCOL:' in body['messages'][0]['content'] or (protocol=='native' and 'Completion review:' in text)
    calls.append(body);reviews+=bool(review)
    def finish(answer):
     return [('complete_task',{'status':'complete','requires_tools':False,'answer':answer})] if protocol=='native' or review else []
    answer='';actions=[]
    if mode!='zero' and phase==0:
     actions=[('file_read',{'path':p+'/README.md'}) for p in ('left','right')];phase=1
    elif not review:
     answer=PROGRESS;actions=finish(answer)
    elif mode=='stuck':
     answer=PROGRESS # Deliberately no decision: must remain FAILED.
    elif phase<2:
     actions=[('file_read',{'path':p+'/source.c'}) for p in ('left','right')];phase=2
    elif phase==2:
     assert 'LEFT_SOURCE_PROOF' in text and 'RIGHT_SOURCE_PROOF' in text
     actions=[('file_write',{'path':'comparison.html','content':HTML})];phase=3
    elif phase==3:
     actions=[('file_read',{'path':'comparison.html'})];phase=4
    else:
     assert HTML in text;answer=FINAL;actions=finish(answer)
    if protocol=='native':
     if len(actions)>1:answer=PROGRESS # Commentary must not hide the executable batch.
     message={'content':answer,'tool_calls':[{'id':f'call-{len(calls)}-{i}','type':'function','function':{'name':name,'arguments':json.dumps(args)}} for i,(name,args) in enumerate(actions)]} if actions else {'content':answer}
    else:message={'content':json.dumps([{'tool':name,'args':args} for name,args in actions],ensure_ascii=False) if actions else answer}
    respond_json(self,json.dumps({'choices':[{'message':message,'finish_reason':'tool_calls' if protocol=='native' and actions else 'stop'}]}).encode())
   except Exception as error:errors.append(repr(error));raise
 server=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=server.serve_forever,daemon=True).start()
 try:
  with tempfile.TemporaryDirectory(prefix='coa-artifact-recovery-') as folder:
   work=Path(folder)
   for directory,proof in [('left','LEFT_SOURCE_PROOF scaling_cur_freq'),('right','RIGHT_SOURCE_PROOF cpuinfo_cur_freq')]:
    (work/directory).mkdir();(work/directory/'README.md').write_text('Project overview only.',encoding='utf8');(work/directory/'source.c').write_text(proof,encoding='utf8')
   (work/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixture','llm.api_key':'fixture','llm.base_url':f'http://127.0.0.1:{server.server_port}','reasoning.max_rounds':20}),encoding='utf8')
   with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
   def api(path,body=None):
    req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=5) as response:return json.load(response)
   with (BUILD/f'artifact-delivery-{protocol}-{mode}.log').open('w',encoding='utf8') as log:
    process=subprocess.Popen([str(BINARY),'--state',folder,'--workspace',folder,'serve',str(port)],cwd=ROOT,env={**os.environ,'COA_TOOL_PROTOCOL':protocol},stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    try:
     deadline=time.monotonic()+25
     for _ in range(150):
      try:api('/v1/scheduler');break
      except OSError:assert process.poll() is None;time.sleep(.05)
     prompt=('对比一下 left 和 right 两个项目的代码，详细解释一下两个项目之间的差异与优缺点，最后也输出一个html来对比说明。'
      if mode!='zero' else '对比 left 和 right 两个项目，生成 comparison.html 文件并读回校验。')
     tid=api('/v1/chat',{'message':prompt,'session':'artifact-recovery'})['id']
     while True:
      task=api(f'/v1/tasks/{tid}')
      if task['status'] not in ('RUNNING','QUEUED'):break
      assert time.monotonic()<deadline,task;time.sleep(.03)
     assert not errors,errors
     assert len(calls)<=10,len(calls)
     if mode=='stuck':
      assert task['status']=='FAILED' and not (work/'comparison.html').exists(),task
      assert not api('/v1/chat/history?session=artifact-recovery')['turns']
     else:
      assert task['status']=='DONE' and task['output']==FINAL,task
      assert reviews and (work/'comparison.html').read_text(encoding='utf8')==HTML
      steps=task['steps'];writes=[i for i,s in enumerate(steps) if s['tool']=='file_write' and s['ok']]
      assert writes and any(i>writes[-1] and s['tool']=='file_read' and s['ok'] and HTML in s['out'] for i,s in enumerate(steps)),steps
      assert all(any(s['tool']=='file_read' and s['ok'] and proof in s['out'] for s in steps) for proof in ('LEFT_SOURCE_PROOF','RIGHT_SOURCE_PROOF')),steps
      assert api('/v1/chat/history?session=artifact-recovery')['turns'][-1]['a']==FINAL
     print('PASS #232:',protocol,mode,task['status'],'requests',len(calls),'tools',task['tool_calls'],flush=True)
    finally:process.terminate();process.wait(timeout=5)
 finally:server.shutdown();server.server_close()
if __name__=='__main__':
 for protocol,mode in [('legacy','recover'),('legacy','zero'),('legacy','stuck'),('native','recover')]:exercise(protocol,mode)
