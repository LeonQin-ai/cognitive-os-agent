"""Native OpenAI/Anthropic tool-block continuation regressions (no paid API)."""
import json,socket,subprocess,tempfile,threading,time,urllib.request,os
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
backend=Path(__file__).resolve().parents[1]
build=backend/os.environ.get('COA_BUILD_DIR','build')
build.mkdir(parents=True,exist_ok=True)
for provider_name in ('openai','anthropic'):
 counts={};catalogs=[]
 class Provider(BaseHTTPRequestHandler):
  def log_message(self,*args):pass
  def do_POST(self):
   body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
   catalogs.append(body.get('tools',[]))
   text=body['messages'][-1]['content']
   current=text.split('## Current request\n',1)[-1].split('\n## ',1)[0]
   case=next(k for k in ('NATIVE_MIXED','NATIVE_FINAL','NATIVE_BAD','NATIVE_EXAMPLE','NATIVE_PENDING','NATIVE_BLOCKED','NATIVE_CHATTER') if k in current)
   n=counts.get(case,0);counts[case]=n+1
   calls=[];answer='';stop='stop'
   if case=='NATIVE_BLOCKED':calls=[('complete_task',{'status':'blocked','requires_tools':False,'answer':'Missing provider credential; user action required.'})]
   elif case=='NATIVE_CHATTER':answer='Zyxwv alpha beta gamma'
   elif case=='NATIVE_PENDING':
    if n==0:calls=[('complete_task',{'status':'complete','requires_tools':True,'answer':'pending'})]
    elif n==1:calls=[('file_write',{'path':'pending.txt','content':'actually written'})]
    else:answer='pending.txt verified.'
   elif case=='NATIVE_EXAMPLE':answer=json.dumps([{'tool':'file_write','args':{'path':'example-only.txt','content':'example'}}])
   elif case=='NATIVE_FINAL':answer='我需要说明：缓存使已读取的内容能够复用，这是最终的原理解释。'
   elif case=='NATIVE_MIXED':
    if n==0:
     answer='已经完成。';calls=[('file_write',{'path':'native.txt','content':'native-marker'}),('complete_task',{'status':'complete','requires_tools':False,'answer':'premature finish'})]
    elif n==1:
     answer='دَعْنِي أتحقق من النتيجة';calls=[('file_read',{'path':'native.txt'})]
    else:answer='已生成并实际读取 native.txt，native-marker。'
   else:
    if n==0:calls=[('file_write',{'path':'must-not-exist.txt','content':'bad-prefix'}),('file_write','{"path":')]
    elif n==1:calls=[('file_write',{'path':'repaired.txt','content':'recovered'})]
    else:answer='已交付 repaired.txt。'
   if not calls and case!='NATIVE_CHATTER':calls=[('complete_task',{'status':'complete','requires_tools':False,'answer':answer})];answer=''
   if provider_name=='openai':
    message={'content':answer or None}
    if calls:message['tool_calls']=[{'id':'c'+str(i),'type':'function','function':{'name':name,'arguments':json.dumps(args) if isinstance(args,dict) else args}} for i,(name,args) in enumerate(calls)]
    response={'choices':[{'message':message,'finish_reason':stop}]}
   else:
    content=([{'type':'text','text':answer}] if answer else [])
    content += [{'type':'tool_use','id':'c'+str(i),'name':name,'input':args} for i,(name,args) in enumerate(calls)]
    response={'content':content,'stop_reason':'end_turn'}
   raw=json.dumps(response).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
 with tempfile.TemporaryDirectory(prefix='coa-native-loop-') as folder:
  state=Path(folder);server=ThreadingHTTPServer(('127.0.0.1',0),Provider)
  threading.Thread(target=server.serve_forever,daemon=True).start()
  (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':provider_name,'llm.model':'native-test','llm.api_key':'local-fixture','llm.base_url':f'http://127.0.0.1:{server.server_port}','reasoning.max_rounds':8}),encoding='utf-8')
  with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
  def request(path,body=None):
   req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
   with urllib.request.urlopen(req,timeout=5) as response:return json.load(response)
  with (build/('native-loop-'+provider_name+'.log')).open('w',encoding='utf-8') as log:
   process=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',str(state),'--workspace',str(state),'serve',str(port)],cwd=backend,stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
   try:
    for _ in range(150):
     try:request('/v1/scheduler');break
     except OSError:assert process.poll() is None;time.sleep(.1)
    def run(prompt):
     task=request('/v1/tasks',{'prompt':prompt});deadline=time.monotonic()+30
     while True:
      t=request('/v1/tasks/'+str(task['id']))
      if t['status'] not in ('RUNNING','QUEUED','CANCELLING'):return t
      assert time.monotonic()<deadline,t;time.sleep(.05)
    mixed=run('生成 native.txt 并读取核对 NATIVE_MIXED')
    assert mixed['status']=='DONE' and counts['NATIVE_MIXED']==3,mixed
    assert (state/'native.txt').read_text()=='native-marker'
    assert [s['tool'] for s in mixed['steps']]==['file_write','file_read'],mixed
    final=run('解释缓存原理 NATIVE_FINAL')
    assert final['status']=='DONE' and counts['NATIVE_FINAL']==1,final
    example=run('解释工具调用格式 NATIVE_EXAMPLE')
    assert example['status']=='DONE' and counts['NATIVE_EXAMPLE']==1 and not (state/'example-only.txt').exists(),example
    bad=run('创建 repaired.txt NATIVE_BAD')
    assert bad['status']=='DONE' and counts['NATIVE_BAD']==3,bad
    assert not (state/'must-not-exist.txt').exists()
    assert (state/'repaired.txt').read_text()=='recovered'
    assert len(bad['steps'])==1,bad
    pending=run('创建 pending.txt NATIVE_PENDING')
    assert pending['status']=='DONE' and counts['NATIVE_PENDING']==3 and (state/'pending.txt').exists(),pending
    blocked=run('处理任务 NATIVE_BLOCKED')
    assert blocked['status']=='FAILED' and counts['NATIVE_BLOCKED']==1 and 'Missing provider credential' in blocked['output'],blocked
    chatter=run('处理任务 NATIVE_CHATTER')
    assert chatter['status']=='FAILED' and 2<=counts['NATIVE_CHATTER']<=5 and not chatter['steps'],chatter
    assert catalogs and all(catalogs)
    for catalog in catalogs:
     write=next(t.get('function',t) for t in catalog if t.get('function',t).get('name')=='file_write')
     schema=write.get('parameters',write.get('input_schema'))
     assert schema['type']=='object' and 'path' in schema['properties'] and 'content' in schema['properties']
    print('PASS:',provider_name,'native blocks override text/stop hints; final text avoids keyword retries; incomplete batch never executes')
   finally:
    process.terminate()
    try:process.wait(timeout=5)
    except subprocess.TimeoutExpired:process.kill();process.wait()
    server.shutdown()
