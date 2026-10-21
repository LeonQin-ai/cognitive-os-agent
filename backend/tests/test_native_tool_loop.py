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
 counts={};catalogs=[];active_case=None;summary_calls={}
 class Provider(BaseHTTPRequestHandler):
  def log_message(self,*args):pass
  def do_POST(self):
   body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
   if not body.get('tools'):
    assert 0<body['max_tokens']<=4096, 'Summary must also respect model output capacity'
    summary_calls[active_case]=summary_calls.get(active_case,0)+1
    response={'choices':[{'message':{'content':'Stopped repeated verification; preserve existing artifacts.'},'finish_reason':'stop'}]} if provider_name=='openai' else {'content':[{'type':'text','text':'Stopped repeated verification; preserve existing artifacts.'}],'stop_reason':'end_turn'}
    raw=json.dumps(response).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw);return
   assert body['max_tokens']==4096, 'Model output capacity must bound the request budget'
   catalogs.append(body.get('tools',[]))
   system=body['messages'][0]['content']
   if isinstance(system,str):
    assert 'Respond with ONLY a JSON array' not in system
    assert '-> [{"tool"' not in system
   case=active_case
   if counts.get(case,0)>0:
    prompt=body['messages'][-1]['content']
    cursor=json.loads(prompt.rsplit('## Current execution state\n',1)[1].split('\n',1)[0])
    assert cursor['round']>=2
    if case in ('NATIVE_RECOVERED','NATIVE_ALIAS') and counts[case]==1:
     assert cursor['attempted_steps']==1 and cursor['last_attempt']['ok']==0
     assert cursor['unresolved_failure_count']==1 and cursor['unresolved_failures'][0]['tool']=='file_read'
   n=counts.get(case,0);counts[case]=n+1
   calls=[];answer='';stop='stop'
   if case.startswith('ANSWER_'):answer=matrix[int(case.split('_')[1])][1]
   elif case in ('NATIVE_READ_PROTOCOL','NATIVE_READ_CONTROL'):
    bad='native.txt</parameter>\n<｜DSML｜ parameter name="offset_bytes" string="false">0' if case=='NATIVE_READ_PROTOCOL' else 'bad\nname.txt'
    if n==0:calls=[('file_read',{'path':bad})]
    elif n==1:
     assert cursor['attempted_steps']==0 and cursor['unresolved_failure_count']==0 and cursor['last_planner_error']
     calls=[('file_read',{'path':'native.txt'})]
    else:answer='native-marker verified.'
   elif case=='NATIVE_WRITE_PROTOCOL':
    if n==0:calls=[('file_write',{'path':'bad-prefix.txt','content':'must not execute'}),('file_write',{'path':'chapter.md</parameter>\n<｜DSML｜ parameter name="content">story'})]
    elif n==1:
     assert cursor['attempted_steps']==0 and cursor['unresolved_failure_count']==0
     calls=[('file_write',{'path':'chapter.md','content':'Literal </parameter>\n<｜DSML｜ parameter name="content"> is document content.'})]
    elif n==2:calls=[('file_read',{'path':'chapter.md'})]
    else:answer='chapter.md written and verified.'
   elif case in ('NATIVE_VERIFY_SPIN','NATIVE_VERIFY_PROGRESS','NATIVE_FRESH_OBSERVATIONS','NATIVE_VERIFY_SHELL_PROGRESS','NATIVE_VERIFY_TOUCH_SPIN','NATIVE_VERIFY_OSCILLATE_SPIN','NATIVE_VERIFY_MANY_ARTIFACTS'):
    if n<40:
     if case=='NATIVE_VERIFY_SPIN' and n>=5:assert cursor['no_progress_warning'] is True,cursor
     calls=[('shell',{'command':f'python {"verify-fresh.py" if case=="NATIVE_FRESH_OBSERVATIONS" else "verify-loop.py"} {n}'})]
     if case=='NATIVE_VERIFY_PROGRESS':calls.insert(0,('file_write',{'path':'progress-artifact.txt','content':f'actual-new-content-{n}'}))
     if case in ('NATIVE_VERIFY_SHELL_PROGRESS','NATIVE_VERIFY_TOUCH_SPIN','NATIVE_VERIFY_OSCILLATE_SPIN','NATIVE_VERIFY_MANY_ARTIFACTS'):
      calls[0][1]['command']+=' '+{'NATIVE_VERIFY_SHELL_PROGRESS':'write','NATIVE_VERIFY_TOUCH_SPIN':'touch','NATIVE_VERIFY_OSCILLATE_SPIN':'oscillate','NATIVE_VERIFY_MANY_ARTIFACTS':'many'}[case]
      calls[0][1]['outputs']=[f'artifact-{n}-{i}.txt' for i in range(4)] if case=='NATIVE_VERIFY_MANY_ARTIFACTS' else ['shell-progress.txt']
    else:answer='All generated content is verified.'
   elif case.startswith('NATIVE_EXTERNAL_'):
    discovery='grep' if case.endswith('GREP') else 'glob'
    if n==0:calls=[('file_read',{'path':(external/'context.h' if case=='NATIVE_EXTERNAL_BASENAME' else state/'external-project-link/addons/rmrs/context.h' if case=='NATIVE_EXTERNAL_ALIAS' else wrong).as_posix()})]
    elif n==1:
     calls=[('glob',{'path':(state/'external-project-link' if case=='NATIVE_EXTERNAL_ALIAS' else external).as_posix(),'pattern':'**/context.h'})] if discovery=='glob' else [('grep',{'path':external.as_posix(),'pattern':'external-recovery-marker','output_mode':'files_with_matches'})]
    elif n==2 and case!='NATIVE_EXTERNAL_DISCOVERY_ONLY':calls=[('file_read',{'path':(decoy if case=='NATIVE_EXTERNAL_DECOY' else actual).as_posix()})]
    else:answer='external-recovery-marker verified.'
   elif case.startswith('NATIVE_TOOL_DIAGNOSTIC_'):
    tool=case.removeprefix('NATIVE_TOOL_DIAGNOSTIC_')
    args={'list_dir':{'path':'missing-directory'},'glob':{'path':'missing-directory','pattern':'*.c'},'grep':{'path':'explanation.txt','pattern':'needle','max_scan_bytes':1},'git':{'dir':str(state),'args':'status --short'},'shell':{'command':'exit 7','timeout_ms':1000},'file_edit':{'path':'native.txt','old_string':'absent-unique','new_string':'large-edit-body'},'file_write':{'path':'blocked-target','content':'large-write-body'},'mcp':{'server':'not-registered','tool':'probe'},'skill':{'name':'not-registered'}}[tool]
    if n==0:calls=[(tool,args)]
    elif n<4:calls=[('file_read',{'path':f'research-{n}.txt'})]
    else:
     pending=next(f for f in cursor['unresolved_failures'] if f['tool']==tool)
     expected={k:v for k,v in args.items() if k not in ('content','old_string','new_string')}
     assert pending['arguments']==expected and pending['error'],pending
     if tool=='git':assert str(state) in pending['error'],pending
     calls=[('complete_task',{'status':'blocked','requires_tools':False,'answer':'Original tool failure remains; diagnostics retained.'})]
   elif case=='NATIVE_DELAYED_RECOVERY':
    if n==0:calls=[('file_read',{'path':'delayed-missing.txt','offset_bytes':19,'max_bytes':64})]
    elif n<5:calls=[('file_read',{'path':f'research-{n}.txt'})]
    elif n==5:
     pending=cursor['unresolved_failures'][0]
     assert pending['arguments']=={'path':'delayed-missing.txt','offset_bytes':19,'max_bytes':64},pending
     assert pending['error'] and 'delayed-missing.txt' in pending['error'],pending
     calls=[('file_write',{'path':'delayed-missing.txt','content':'x'*19+'recovered-delayed'})]
    elif n==6:calls=[('file_read',{'path':'delayed-missing.txt','offset_bytes':19,'max_bytes':64})]
    else:answer='recovered-delayed verified after unrelated research.'
   elif case=='NATIVE_UNRELATED_WRITE':
    if n==0:calls=[('file_write',{'path':'blocked-target','content':'must be written'})]
    elif n==1:calls=[('file_write',{'path':'unrelated.txt','content':'other file'})]
    else:answer='Only the unrelated file succeeded; original target is unresolved.'
   elif case=='NATIVE_EMPTY_WRITE':
    if n==0:calls=[('file_write',{'path':'explicit-empty.txt','content':''})]
    else:answer='Empty file created as requested.'
   elif case in ('NATIVE_IPC_PROGRESS','NATIVE_IPC_PENDING','NATIVE_IPC_PREMATURE'):
    if n==0:calls=[('file_read',{'path':'native.txt'})]
    elif n==1:
     answer='基于已收集的大量代码证据，我需要再补充读取几个关键的 IPC 接口定义文件，以完整分析服务端与客户端的通信机制。'
     if case=='NATIVE_IPC_PENDING':calls=[('complete_task',{'status':'complete','requires_tools':True,'answer':answer})];answer=''
    elif n==2:
     if case=='NATIVE_IPC_PREMATURE':assert 'Completion review:' in body['messages'][-1]['content']
     calls=[('file_read',{'path':'ipc.h'})]
    else:answer='服务端接收请求并发送响应，客户端发起调用；ipc.h 定义 IPC_REQUEST 和 IPC_RESPONSE 接口。'
   elif case=='NATIVE_PARTIAL':
    if n==0:calls=[('file_read',{'path':'explanation.txt','max_bytes':32})]
    else:answer='注释中的完整阅读是概念，本次片段足够解释，没有要求读取剩余内容。'
   elif case=='NATIVE_RESEARCH':
    if n<17:calls=[('file_read',{'path':f'research-{n}.txt'})]
    else:answer='17份样例均已核对，这是只读分析结果，无需创建文件。'
   elif case=='NATIVE_SCHEMA':
    if n==0:calls=[('file_write',{'path':'schema-prefix.txt','content':'must not execute'}),('file_read',{'tool':'file_read','args':{'path':'schema.txt'}})]
    elif n==1:calls=[('file_write',{'path':'schema.txt','content':'schema-repaired'})]
    elif n==2:calls=[('file_read',{'path':'schema.txt'})]
    else:answer='参数已校正并成功核对 schema-repaired。'
   elif case=='NATIVE_FAILED':
    if n==0:calls=[('file_read',{'path':'unresolved-missing.txt'})]
    else:answer='模型声称成功，但实际文件读取仍然失败。'+'技术分析正文。'*2000+'SUMMARY_END_194'
   elif case in ('NATIVE_RECOVERED','NATIVE_ALIAS'):
    path='recover.txt' if case=='NATIVE_RECOVERED' else (state/'alias'/'alias-recover.txt').as_posix()
    if n==0:calls=[('file_read',{'path':path})]
    elif n==1:calls=[('file_write',{'path':path,'content':'recovered-marker'})]
    elif n==2:calls=[('file_read',{'path':path})]
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
   if not calls and case!='NATIVE_CHATTER' and not (case=='NATIVE_IPC_PROGRESS' and n==1):calls=[('complete_task',{'status':'complete','requires_tools':False,'answer':answer})];answer=''
   if provider_name=='openai':
    message={'content':answer or None}
    if calls:message['tool_calls']=[{'id':'c'+str(i),'type':'function','function':{'name':name,'arguments':json.dumps(args) if isinstance(args,dict) else args}} for i,(name,args) in enumerate(calls)]
    response={'choices':[{'message':message,'finish_reason':stop}]}
   else:
    content=([{'type':'text','text':answer}] if answer else [])
    content += [{'type':'tool_use','id':'c'+str(i),'name':name,'input':args} for i,(name,args) in enumerate(calls)]
    response={'content':content,'stop_reason':'end_turn'}
   raw=json.dumps(response).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
 with tempfile.TemporaryDirectory(prefix='coa-native-loop-') as folder, tempfile.TemporaryDirectory(prefix='coa-native-project-') as external_folder:
  external=Path(external_folder)
  actual=external/'src/addons/rmrs/context.h';actual.parent.mkdir(parents=True);actual.write_text('external-recovery-marker')
  wrong=external/'addons/rmrs/context.h'
  decoy=external/'src/other/context.h';decoy.parent.mkdir(parents=True);decoy.write_text('unrelated')
  state=Path(folder);server=ThreadingHTTPServer(('127.0.0.1',0),Provider)
  threading.Thread(target=server.serve_forever,daemon=True).start()
  (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':provider_name,'llm.model':'native-test','llm.api_key':'local-fixture','llm.base_url':f'http://127.0.0.1:{server.server_port}','reasoning.max_rounds':-1}),encoding='utf-8')
  (state/'routes.json').write_text(json.dumps([{'name':'fixture-metadata','provider':provider_name,'model':'native-test','base_url':f'http://127.0.0.1:{server.server_port}','api_key':'local-fixture','context_window':1048576,'max_output_tokens':4096,'context_source':'provider_metadata'}]),encoding='utf-8')
  if os.name!='nt':
   (state/'alias').symlink_to(state,target_is_directory=True)
   (state/'external-project-link').symlink_to(external,target_is_directory=True)
  else:(state/'alias').mkdir()
  (state/'verify-loop.py').write_text('import sys\nn=int(sys.argv[1])\nprint("#< CLIXML" if n%2 else "")\nprint("\\n".join("chapter-%02d: complete verified content with stable names and structure"%i for i in range(20)))\nprint("EXIT_LABEL_%d=0"%n)\nif n%2:print(\'<Objs Version="1.1.0.1"><Obj S="progress" RefId="%d"><MS>transient-%d</MS></Obj></Objs>\'%(n,n))\n\nif len(sys.argv)>2:\n open("shell-progress.txt","w").write(str(n) if sys.argv[2]=="write" else str(n%2) if sys.argv[2]=="oscillate" else "same-content")\n\nif len(sys.argv)>2 and sys.argv[2]=="many":\n for i in range(4):open("artifact-%d-%d.txt"%(n,i),"w").write("generated-%d-%d"%(n,i))\n',encoding='utf-8')
  (state/'verify-fresh.py').write_text('import hashlib,sys\nn=int(sys.argv[1])\nprint("\\n".join(hashlib.sha256((str(n)+":"+str(i)).encode()).hexdigest() for i in range(20)))\n',encoding='utf-8')
  (state/'blocked-target').mkdir()
  (state/'ipc.h').write_text('enum { IPC_REQUEST=31, IPC_RESPONSE=32 };',encoding='utf-8')
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
    assert mixed['status']=='DONE' and counts['NATIVE_MIXED']==4,mixed
    assert (state/'native.txt').read_text()=='native-marker'
    assert [s['tool'] for s in mixed['steps']]==['file_write','file_read'],mixed
    for case in ('NATIVE_READ_PROTOCOL','NATIVE_READ_CONTROL'):
     repaired=run(case,'读取文件核对标记。')
     assert repaired['status']=='DONE' and counts[case]==4 and len(repaired['steps'])==1 and repaired['steps'][0]['ok'],repaired
    repaired=run('NATIVE_WRITE_PROTOCOL','保存一份含有协议示例文字的文档，并读取核对。')
    assert repaired['status']=='DONE' and counts['NATIVE_WRITE_PROTOCOL']==5 and len(repaired['steps'])==2,repaired
    assert not (state/'bad-prefix.txt').exists()
    assert (state/'chapter.md').read_text(encoding='utf-8')=='Literal </parameter>\n<｜DSML｜ parameter name="content"> is document content.'
    empty=run('NATIVE_EMPTY_WRITE','创建一个空文件。')
    assert empty['status']=='DONE' and counts['NATIVE_EMPTY_WRITE']==3 and (state/'explicit-empty.txt').read_bytes()==b'',empty
    spin=run('NATIVE_VERIFY_SPIN','执行校验并按真实进展收尾。')
    assert spin['status']=='FAILED' and counts['NATIVE_VERIFY_SPIN']<=12,{k:spin.get(k) for k in ('status','round','tool_calls')}
    assert summary_calls.get('NATIVE_VERIFY_SPIN')==1,summary_calls
    assert spin['llm_calls']==counts['NATIVE_VERIFY_SPIN']+1,spin['llm_calls']
    advancing=run('NATIVE_VERIFY_PROGRESS','持续生成新内容并校验，全部完成后交付。')
    assert advancing['status']=='DONE' and counts['NATIVE_VERIFY_PROGRESS']==42,advancing
    assert (state/'progress-artifact.txt').read_text()=='actual-new-content-39'
    shell_progress=run('NATIVE_VERIFY_SHELL_PROGRESS','用脚本持续生成实际新内容。')
    assert shell_progress['status']=='DONE' and counts['NATIVE_VERIFY_SHELL_PROGRESS']==42,{k:shell_progress.get(k) for k in ('status','round','tool_calls')}
    assert (state/'shell-progress.txt').read_text()=='39'
    touch=run('NATIVE_VERIFY_TOUCH_SPIN','反复校验同一个内容版本。')
    assert touch['status']=='FAILED' and counts['NATIVE_VERIFY_TOUCH_SPIN']<=12,{k:touch.get(k) for k in ('status','round','tool_calls')}
    oscillate=run('NATIVE_VERIFY_OSCILLATE_SPIN','不能把来回重写旧版本当作新进展。')
    assert oscillate['status']=='FAILED' and counts['NATIVE_VERIFY_OSCILLATE_SPIN']<=12,{k:oscillate.get(k) for k in ('status','round','tool_calls')}
    many=run('NATIVE_VERIFY_MANY_ARTIFACTS','分阶段生成160个不同文件。')
    assert many['status']=='DONE' and counts['NATIVE_VERIFY_MANY_ARTIFACTS']==42,{k:many.get(k) for k in ('status','round','tool_calls')}
    assert len(list(state.glob('artifact-*.txt')))==160
    fresh=run('NATIVE_FRESH_OBSERVATIONS','持续收集新的观察证据，再完成分析。')
    assert fresh['status']=='DONE' and counts['NATIVE_FRESH_OBSERVATIONS']==42,{k:fresh.get(k) for k in ('status','round','tool_calls')}
    for case in ('NATIVE_EXTERNAL_GLOB','NATIVE_EXTERNAL_GREP','NATIVE_EXTERNAL_DECOY','NATIVE_EXTERNAL_DISCOVERY_ONLY','NATIVE_EXTERNAL_BASENAME','NATIVE_EXTERNAL_ALIAS'):
     if case=='NATIVE_EXTERNAL_ALIAS' and os.name=='nt':continue
     relocated=run(case,'定位并读取目标项目文件。')
     success=case in ('NATIVE_EXTERNAL_GLOB','NATIVE_EXTERNAL_GREP','NATIVE_EXTERNAL_ALIAS')
     assert relocated['status']==('DONE' if success else 'FAILED'),relocated
     assert counts[case]<=(5 if success else 6),relocated
    for tool in ('list_dir','grep','git','shell','file_edit','file_write','mcp','skill'):
     diagnostic=run('NATIVE_TOOL_DIAGNOSTIC_'+tool,'检查工具失败后的恢复诊断。')
     assert diagnostic['status']=='FAILED' and counts['NATIVE_TOOL_DIAGNOSTIC_'+tool]==5,diagnostic
     if tool=='file_write':assert (state/'blocked-target').is_dir(), 'Failed write rollback removed existing directory'
    delayed=run('NATIVE_DELAYED_RECOVERY','核对目标文件，完成相关资料研究。')
    assert delayed['status']=='DONE' and counts['NATIVE_DELAYED_RECOVERY']==9,delayed
    unrelated=run('NATIVE_UNRELATED_WRITE','写入目标并核对。')
    assert unrelated['status']=='FAILED' and counts['NATIVE_UNRELATED_WRITE']<=5 and any(not st['ok'] for st in unrelated['steps']),unrelated
    final=run('NATIVE_FINAL','解释缓存原理')
    assert final['status']=='DONE' and counts['NATIVE_FINAL']==1,final
    capability=run('NATIVE_CAPABILITY','你有啥特殊的，创建、安装、修复这些能力有哪些特点？')
    assert capability['status']=='DONE' and counts['NATIVE_CAPABILITY']==1 and not capability['steps'],capability
    for i,(prompt,answer) in enumerate(matrix):
     case=f'ANSWER_{i}';task=run(case,prompt)
     assert task['status']=='DONE' and counts[case]==1 and not task['steps'] and task['output']==answer,(prompt,task)
    for case in ('NATIVE_IPC_PROGRESS','NATIVE_IPC_PENDING','NATIVE_IPC_PREMATURE'):
     ipc=run(case,'分析代码中的服务端和客户端。')
     assert ipc['status']=='DONE' and counts[case]==(4 if case=='NATIVE_IPC_PREMATURE' else 5),ipc
     assert [st['tool'] for st in ipc['steps']]==['file_read','file_read'] and 'IPC_REQUEST' in ipc['output'],ipc
    partial=run('NATIVE_PARTIAL','阅读样例文件的相关片段，解释注释中的“完整读取全部源码”是什么意思。')
    assert partial['status']=='DONE' and counts['NATIVE_PARTIAL']==3 and len(partial['steps'])==1,partial
    research=run('NATIVE_RESEARCH','请分析样例中的 create operation，提供技术说明。')
    assert research['status']=='DONE' and counts['NATIVE_RESEARCH']==19 and len(research['steps'])==17,research
    recovered=run('NATIVE_RECOVERED','处理目标文件并核对最终内容。')
    assert recovered['status']=='DONE' and counts['NATIVE_RECOVERED']==5,recovered
    alias=run('NATIVE_ALIAS','处理目录别名下的目标文件并核对结果。')
    assert alias['status']=='DONE' and counts['NATIVE_ALIAS']==5,alias
    schema=run('NATIVE_SCHEMA','处理目标文件并核对内容。')
    assert schema['status']=='DONE' and counts['NATIVE_SCHEMA']==5 and len(schema['steps'])==2,schema
    assert not (state/'schema-prefix.txt').exists()
    assert all(st['ok'] for st in schema['steps']),schema
    failed=run('NATIVE_FAILED','读取指定文件并总结。')
    assert failed['status']=='FAILED' and 2<=counts['NATIVE_FAILED']<=3 and any(not st['ok'] for st in failed['steps']),failed
    assert failed['output'].endswith('SUMMARY_END_194') and failed['output'].count('技术分析正文。')==2000,failed
    example=run('NATIVE_EXAMPLE','解释工具调用格式')
    assert example['status']=='DONE' and counts['NATIVE_EXAMPLE']==1 and not (state/'example-only.txt').exists(),example
    bad=run('NATIVE_BAD','创建 repaired.txt')
    assert bad['status']=='DONE' and counts['NATIVE_BAD']==4,bad
    assert not (state/'must-not-exist.txt').exists()
    assert (state/'repaired.txt').read_text()=='recovered'
    assert len(bad['steps'])==1,bad
    discovery=run('NATIVE_DISCOVERY','仅搜索文件名，不读文件，不写文件')
    assert discovery['status']=='DONE' and counts['NATIVE_DISCOVERY']==3 and [s['tool'] for s in discovery['steps']]==['grep'],discovery
    pending=run('NATIVE_PENDING','创建 pending.txt')
    assert pending['status']=='DONE' and counts['NATIVE_PENDING']==4 and (state/'pending.txt').exists(),pending
    blocked=run('NATIVE_BLOCKED','处理任务')
    assert blocked['status']=='FAILED' and counts['NATIVE_BLOCKED']==1 and 'Missing provider credential' in blocked['output'],blocked
    chatter=run('NATIVE_CHATTER','处理任务')
    assert chatter['status']=='FAILED' and 2<=counts['NATIVE_CHATTER']<=5 and not chatter['steps'],chatter
    assert catalogs and all(catalogs)
    for catalog in catalogs:
     write=next(t.get('function',t) for t in catalog if t.get('function',t).get('name')=='file_write')
     schema=write.get('parameters',write.get('input_schema'))
     assert schema['type']=='object' and set(schema['required'])=={'path','content'}
     assert 'format' not in schema['properties']['path']
    print('PROGRESS:',provider_name,{'spin_planning_calls':counts['NATIVE_VERIFY_SPIN'],'spin_summary_calls':summary_calls['NATIVE_VERIFY_SPIN'],'new_content_calls':counts['NATIVE_VERIFY_PROGRESS'],'new_evidence_calls':counts['NATIVE_FRESH_OBSERVATIONS'],'shell_artifact_calls':counts['NATIVE_VERIFY_SHELL_PROGRESS'],'touch_spin_calls':counts['NATIVE_VERIFY_TOUCH_SPIN'],'oscillate_spin_calls':counts['NATIVE_VERIFY_OSCILLATE_SPIN'],'many_artifact_calls':counts['NATIVE_VERIFY_MANY_ARTIFACTS']})
    print('PASS:',provider_name,f'{len(matrix)} ordinary answers + premature completion review, partial reading, 17-step research, recovered/real failures; fixture routing is out of band, no magic prompt tags')
   except BaseException:
    log.flush()
    print((build/('native-loop-'+provider_name+'.log')).read_text(encoding='utf-8',errors='replace')[-16000:],flush=True)
    raise
   finally:
    process.terminate()
    try:process.wait(timeout=5)
    except subprocess.TimeoutExpired:process.kill();process.wait()
    server.shutdown()
