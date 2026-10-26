"""Opt-in #218/#221 actual configured API; runtime creates all resulting edits."""
import argparse,json,os,re,socket,subprocess,tempfile,time,urllib.request,shutil
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--config',required=True);a=p.parse_args();root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
config=json.loads(Path(a.config).read_text(encoding='utf-8-sig'));report={'model':config.get('llm.model'),'cases':[]};out=build/'issue-regressions-live'/str(int(time.time()));out.mkdir(parents=True)
with tempfile.TemporaryDirectory(prefix='coa-private-live-') as temp:
 state=Path(temp);config.update({'auth.key':'','http.bind':'127.0.0.1','reasoning.max_rounds':12});(state/'cognitive-os-agent.json').write_text(json.dumps(config),encoding='utf8')
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 def req(method,path,body=None):
  r=urllib.request.Request(f'http://127.0.0.1:{port}'+path,method=method,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
  with urllib.request.urlopen(r,timeout=10) as response:return json.load(response)
 with (out/'runtime.log').open('w',encoding='utf8') as log:
  proc=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',str(state),'--workspace',str(out),'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0));tid=None
  try:
   for _ in range(100):
    try:req('GET','/v1/scheduler');break
    except OSError:assert proc.poll() is None;time.sleep(.05)
   for case in ('relocated','html'):
    work=out/case;work.mkdir();details={};task=None
    if case=='relocated':
     source=work/'nested/tree/framework/storage/module.h';source.parent.mkdir(parents=True);source.write_text('struct Store { int dirty; };\ninline int flush(Store *s) { int v=s->dirty; s->dirty=0; return v; }\n')
     text='只读分析持久化代码。为复现路径恢复，先实际调用 file_read(path="framework/storage/module.h")，该猜测会失败；随后必须读取正确路径 nested/tree/framework/storage/module.h，根据成功读到的实际源码解释 flush 的行为并完成。不要创建文件，不要反复重试不存在路径。'
    else:
     (work/'comparison.html').write_text('<style>.code-block { overflow-x: auto; }</style><div class="code-block"><span>first line\nsecond line\n+---+\n| A |\n+---+</span></div>',encoding='utf8')
     correct=b'<pre><code>first line\nsecond line\n</code></pre>';(work/'analysis.html').write_bytes(correct)
     text='修复 comparison.html 中代码块的换行显示：保持源码换行和 ASCII 框图对齐，超宽时横向滚动。同时实际读取并核对 analysis.html；若它本来正常就保持原样，不要为了修改而修改。实际修改并读取验证后完成，最后报告两个文件的处理结果，不能把后续计划当作已完成。'
    req('POST','/v1/workspaces',{'path':str(work.resolve())});started=time.monotonic();tid=req('POST','/v1/chat',{'message':text,'session':'issue-'+case,'workspace':str(work.resolve()),'permission_mode':'full'})['id']
    while True:
     task=req('GET','/v1/tasks/'+str(tid))
     if task['status'] not in ('QUEUED','RUNNING'):break
     if time.monotonic()-started>120:req('DELETE','/v1/tasks/'+str(tid));raise AssertionError('live verification time budget exceeded')
     time.sleep(.2)
    (out/(case+'-task.json')).write_text(json.dumps(task,ensure_ascii=False),encoding='utf8')
    if case=='relocated':
     steps=task.get('steps',[]);details={'failed_read':any(s['tool']=='file_read' and s['ok']==0 for s in steps),'recovered_read':any(s['tool']=='file_read' and s['ok']==1 for s in steps)}
    else:
     node=os.environ.get('COA_NODE_EXE') or shutil.which('node') or str(Path.home()/'.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node.exe')
     env={**os.environ,'NODE_PATH':os.environ.get('NODE_PATH') or str(Path(node).parent.parent/'node_modules')}
     layout=json.loads(subprocess.check_output([node,str(root/'tests/verify_html_layout.cjs'),str((work/'comparison.html').resolve())],env=env,text=True))
     details={'preserves_lines':layout['whiteSpace']=='pre' and layout['overflowX']=='auto' and layout['lines']>=5,'correct_file_unchanged':(work/'analysis.html').read_bytes()==correct,'files_read':sum(s['tool']=='file_read' and s['ok']==1 for s in task.get('steps',[]))}
    row={'case':case,'status':task['status'],'elapsed_seconds':round(time.monotonic()-started,3),'model_calls':task.get('llm_calls'),'tool_calls':task.get('tool_calls'),'tokens_in':task.get('tokens_in'),'tokens_out':task.get('tokens_out'),'model_failures':task.get('model_failures'),**details};report['cases'].append(row);report['usage']=req('GET','/v1/usage')
    assert task['status']=='DONE' and all(details.values()),row
    tid=None
  finally:
   if tid is not None:
    try:req('DELETE','/v1/tasks/'+str(tid))
    except OSError:pass
   report['artifact_directory']=str(out);(out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8');(build/'issue-regressions-live-report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8');proc.terminate();proc.wait(timeout=10)
print(json.dumps(report,ensure_ascii=False))
