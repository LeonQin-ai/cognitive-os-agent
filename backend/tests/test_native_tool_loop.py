"""Native OpenAI/Anthropic tool-block continuation regressions (no paid API)."""
import json,socket,subprocess,tempfile,threading,time,urllib.request,os
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
backend=Path(__file__).resolve().parents[1]
build=backend/os.environ.get('COA_BUILD_DIR','build')
build.mkdir(parents=True,exist_ok=True)
matrix=[
 ('你好','你好！很高兴与你交流。'),
 ('你有什么特殊功能','我能帮助解释技术、整理资料和协助使用工具。'),
 ('你有什么特殊功能？','我可以介绍可用能力，以及执行任务时的权限边界。'),
 ('你有啥特殊的，创建、安装、修复这些能力有哪些特点？','这些是可用能力的介绍，无需现在执行安装或修复。'),
 ('解释“完整阅读全部源代码”这句话的含义，不要读取文件。','这句话指读完源码；本次仅解释概念，未读取源码，也不需要读取。'),
 ('把“创建文件后删除它”翻译成英语。','Create the file, then delete it.'),
 ('Compare create, edit, and delete operations conceptually.','Create adds data, edit changes it, and delete removes it.'),
 ('解释这段JSON，不要执行：[{"tool":"file_write","args":{"path":"example.txt"}}]','这是工具调用数据示例，不代表本次实际写入文件。'),
 ('只给出如何检查机器状态的步骤，不要实际执行。','先观察CPU和内存，再查看磁盘和服务状态。'),
 ('用中文写一段包含“执行失败”的日志格式说明。','“执行失败”在这里是日志内容示例，并非本次任务状态。'),
 ('请给出目录遍历算法的伪代码。','function visit(node): for child in node.children: visit(child)'),
 ('Pourquoi utiliser un cache ?','Un cache réutilise les résultats disponibles pour éviter du travail répété.'),
 ('请直接回答2+2等于多少。','4'),
 ('请介绍风险和权限限制，不做任何系统修改。','我需要说明：执行工具受权限约束，说明边界不需要实际修改系统。'),
]
for provider_name in ('openai','anthropic'):
 counts={};catalogs=[];active_case=None
 class Provider(BaseHTTPRequestHandler):
  def log_message(self,*args):pass
  def do_POST(self):
   body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
   catalogs.append(body.get('tools',[]))
   case=active_case
   n=counts.get(case,0);counts[case]=n+1
   calls=[];answer='';stop='stop'
   if case.startswith('ANSWER_'):answer=matrix[int(case.split('_')[1])][1]
   elif case=='NATIVE_PARTIAL':
    if n==0:calls=[('file_read',{'path':'explanation.txt','max_bytes':32})]
    else:answer='注释中的完整阅读是概念，本次片段足够解释，没有要求读取剩余内容。'
   elif case=='NATIVE_RESEARCH':
    if n<17:calls=[('file_read',{'path':f'research-{n}.txt'})]
    else:answer='17份样例均已核对，这是只读分析结果，无需创建文件。'
   elif case=='NATIVE_FAILED':
    if n==0:calls=[('file_read',{'path':'unresolved-missing.txt'})]
    else:answer='模型声称成功，但实际文件读取仍然失败。'
   elif case=='NATIVE_RECOVERED':
    if n==0:calls=[('file_read',{'path':'recover.txt'})]
    elif n==1:calls=[('file_write',{'path':'recover.txt','content':'recovered-marker'})]
    elif n==2:calls=[('file_read',{'path':'recover.txt'})]
    else:answer='已修复并实际核对 recovered-marker。'
   elif case=='NATIVE_DISCOVERY':
    if n==0:calls=[('grep',{'path':'native.txt','pattern':'native-marker','output_mode':'files_with_matches'})]
    else:answer='命中文件 native.txt；没有读取文件，也没有写入文件。'
   elif case=='NATIVE_BLOCKED':calls=[('complete_task',{'status':'blocked','requires_tools':False,'answer':'Missing provider credential; user action required.'})]
   elif case=='NATIVE_CHATTER':answer='Zyxwv alpha beta gamma'
   elif case=='NATIVE_PENDING':
    if n==0:calls=[('complete_task',{'status':'complete','requires_tools':True,'answer':'pending'})]
    elif n==1:calls=[('file_write',{'path':'pending.txt','content':'actually written'})]
    else:answer='pending.txt verified.'
   elif case=='NATIVE_EXAMPLE':answer=json.dumps([{'tool':'file_write','args':{'path':'example-only.txt','content':'example'}}])
   elif case=='NATIVE_CAPABILITY':answer='我能够协助创建内容、解释安装流程和分析修复方案；这是能力介绍，无需实际执行操作。'
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
  (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':provider_name,'llm.model':'native-test','llm.api_key':'local-fixture','llm.base_url':f'http://127.0.0.1:{server.server_port}','reasoning.max_rounds':32}),encoding='utf-8')
  (state/'explanation.txt').write_text('Example comment: full source reading.\n'+'x'*5000,encoding='utf-8')
  for i in range(17):(state/f'research-{i}.txt').write_text(f'unique-example-{i}',encoding='utf-8')
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
    def run(case,prompt):
     global active_case
     active_case=case
     task=request('/v1/chat',{'message':prompt,'session':'fixture-'+case});deadline=time.monotonic()+30
     while True:
      t=request('/v1/tasks/'+str(task['id']))
      if t['status'] not in ('RUNNING','QUEUED','CANCELLING'):return t
      assert time.monotonic()<deadline,t;time.sleep(.05)
    mixed=run('NATIVE_MIXED','生成 native.txt 并读取核对')
    assert mixed['status']=='DONE' and counts['NATIVE_MIXED']==3,mixed
    assert (state/'native.txt').read_text()=='native-marker'
    assert [s['tool'] for s in mixed['steps']]==['file_write','file_read'],mixed
    final=run('NATIVE_FINAL','解释缓存原理')
    assert final['status']=='DONE' and counts['NATIVE_FINAL']==1,final
    capability=run('NATIVE_CAPABILITY','你有啥特殊的，创建、安装、修复这些能力有哪些特点？')
    assert capability['status']=='DONE' and counts['NATIVE_CAPABILITY']==1 and not capability['steps'],capability
    for i,(prompt,answer) in enumerate(matrix):
     case=f'ANSWER_{i}';task=run(case,prompt)
     assert task['status']=='DONE' and counts[case]==1 and not task['steps'] and task['output']==answer,(prompt,task)
    partial=run('NATIVE_PARTIAL','阅读样例文件的相关片段，解释注释中的“完整读取全部源码”是什么意思。')
    assert partial['status']=='DONE' and counts['NATIVE_PARTIAL']==2 and len(partial['steps'])==1,partial
    research=run('NATIVE_RESEARCH','请分析样例中的 create operation，提供技术说明。')
    assert research['status']=='DONE' and counts['NATIVE_RESEARCH']==18 and len(research['steps'])==17,research
    recovered=run('NATIVE_RECOVERED','处理目标文件并核对最终内容。')
    assert recovered['status']=='DONE' and counts['NATIVE_RECOVERED']==4,recovered
    failed=run('NATIVE_FAILED','读取指定文件并总结。')
    assert failed['status']=='FAILED' and 2<=counts['NATIVE_FAILED']<=3 and any(not st['ok'] for st in failed['steps']),failed
    example=run('NATIVE_EXAMPLE','解释工具调用格式')
    assert example['status']=='DONE' and counts['NATIVE_EXAMPLE']==1 and not (state/'example-only.txt').exists(),example
    bad=run('NATIVE_BAD','创建 repaired.txt')
    assert bad['status']=='DONE' and counts['NATIVE_BAD']==3,bad
    assert not (state/'must-not-exist.txt').exists()
    assert (state/'repaired.txt').read_text()=='recovered'
    assert len(bad['steps'])==1,bad
    discovery=run('NATIVE_DISCOVERY','仅搜索文件名，不读文件，不写文件')
    assert discovery['status']=='DONE' and counts['NATIVE_DISCOVERY']==2 and [s['tool'] for s in discovery['steps']]==['grep'],discovery
    pending=run('NATIVE_PENDING','创建 pending.txt')
    assert pending['status']=='DONE' and counts['NATIVE_PENDING']==3 and (state/'pending.txt').exists(),pending
    blocked=run('NATIVE_BLOCKED','处理任务')
    assert blocked['status']=='FAILED' and counts['NATIVE_BLOCKED']==1 and 'Missing provider credential' in blocked['output'],blocked
    chatter=run('NATIVE_CHATTER','处理任务')
    assert chatter['status']=='FAILED' and 2<=counts['NATIVE_CHATTER']<=5 and not chatter['steps'],chatter
    assert catalogs and all(catalogs)
    for catalog in catalogs:
     write=next(t.get('function',t) for t in catalog if t.get('function',t).get('name')=='file_write')
     schema=write.get('parameters',write.get('input_schema'))
     assert schema['type']=='object' and 'path' in schema['properties'] and 'content' in schema['properties']
    print('PASS:',provider_name,f'{len(matrix)} ordinary answers + partial reading, 17-step research, recovered/real failures; fixture routing is out of band, no magic prompt tags')
   finally:
    process.terminate()
    try:process.wait(timeout=5)
    except subprocess.TimeoutExpired:process.kill();process.wait()
    server.shutdown()
