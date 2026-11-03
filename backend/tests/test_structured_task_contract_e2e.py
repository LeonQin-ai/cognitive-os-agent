"""#236: task prose never substitutes for structured decisions or receipts."""
import json, os, socket, subprocess, tempfile, threading, time, urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from fixture_http_response import respond_json

ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/os.environ.get('COA_BUILD_DIR','build')
BINARY=BUILD/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
CONTENT='ACTUAL_ARTIFACT_PROOF'
FINAL='I will read is a quoted phrase. 已完成；检查依据在实际执行记录中。'
def action(name,**args):return {'tool':name,'args':args}
def finish(**extra):return action('complete_task',status='complete',requires_tools=False,answer=FINAL,acceptance={},**extra)
def declare(**acceptance):return action('declare_task',acceptance=acceptance)

def exercise(protocol,mode,prompt='Verify the requested result.'):
    calls=0;acceptance=None
    write=action('file_write',path='report.any',content=CONTENT)
    read=action('file_read',path='report.any')
    done=finish()
    if mode=='plain_final':replies=[FINAL];expected='DONE'
    elif mode=='chat':replies=[done];expected='DONE'
    elif mode=='prefixed':replies=['我先检查文件。<tool_call>file_read{\"path\":\"tiny\"}',done];expected='DONE'
    elif mode=='quoted_commentary':replies=['Example: `<tool_call>shell{} `. Next I will read.',action('file_read',path='tiny'),done];expected='DONE'
    elif mode=='commentary':replies=['Changing wording does not finish a task.','I will inspect the file.',action('file_read',path='tiny'),done];expected='DONE'
    elif mode=='stuck':replies=['准备继续。','The next phase is still pending.','还需要继续查看。'];expected='FAILED'
    elif mode in ('artifact','missing','erase','preexisting','write_only','stale'):
        replies=[declare(artifacts=['report.any'])]
        if mode in ('artifact','write_only','stale'):replies += [write]
        if mode in ('artifact','preexisting','stale'):replies += [read]
        if mode=='stale':replies += [action('file_write',path='report.any',content=CONTENT+' changed')]
        replies += [done];expected='DONE' if mode=='artifact' else 'FAILED'
    elif mode in ('edit','edit_failed','edit_stale'):
        replies=[declare(artifacts=['report.any']),action('file_edit',path='report.any',old_string='OLD_ARTIFACT' if mode!='edit_failed' else 'ABSENT',new_string=CONTENT),read]
        if mode=='edit_stale':replies += [action('file_edit',path='report.any',old_string=CONTENT,new_string='LATER_VERSION')]
        replies += [done];expected='DONE' if mode=='edit' else 'FAILED'
    elif mode in ('full','partial','small','optional'):
        path='tiny' if mode=='small' else 'long.dat'
        if mode!='optional':acceptance={'full_reads':[path]}
        replies=[action('file_read',path=path)]
        if mode=='full':replies += [action('file_read',path=path,offset_bytes=4000),action('file_read',path=path,offset_bytes=8000)]
        replies += [done];expected='FAILED' if mode=='partial' else 'DONE'
    elif mode=='api_missing':acceptance={'artifacts':['report.any']};replies=[done];expected='FAILED'
    elif mode in ('script_output','script_reuse'):
        # Shell-declared artifact: the command writes the file and declares it
        # in outputs[]; the version-matched read back must satisfy acceptance.
        replies=[declare(artifacts=['report.any']),
                 action('shell',command="printf 'ACTUAL_ARTIFACT_PROOF' > report.any",outputs=['report.any']),
                 read,done];expected='DONE'
    elif mode=='script_unchanged':
        # Declaring outputs on a read-only command must not fake a write: the
        # file never changed after the obligation was merged, so the gate holds.
        replies=[declare(artifacts=['report.any']),
                 action('shell',command='cat report.any',outputs=['report.any']),
                 read,done];expected='FAILED'
    elif mode=='script_rewrite':
        replies=[declare(artifacts=['report.any']),write,
                 action('shell',command="printf 'SCRIPT_REPLACED_CONTENT' > report.any",outputs=['report.any']),read,done];expected='DONE'
    elif mode=='unreceipted_rewrite':
        replies=[declare(artifacts=['report.any']),write,
                 action('shell',command="printf 'UNRECEIPTED_VERSION' > report.any"),read,done];expected='FAILED'
    elif mode=='late_output_declaration':
        replies=[declare(artifacts=['report.any']),
                 action('shell',command="printf 'UNRECEIPTED_VERSION' > report.any"),read,
                 action('shell',command='cat report.any',outputs=['report.any']),done];expected='FAILED'
    elif mode=='nonproducing_output_declaration':
        replies=[declare(artifacts=['report.any']),
                 action('shell',command="printf 'UNRECEIPTED_VERSION' > report.any"),
                 action('shell',command='cat report.any',outputs=['report.any']),read,done];expected='FAILED'
    elif mode=='baseline_capacity':
        sources=[f'source-{i}.txt' for i in range(64)]
        replies=[declare(full_reads=sources,artifacts=['report.any'])]
        replies += [[action('file_read',path=path) for path in sources[i:i+8]] for i in range(0,64,8)]
        replies += [action('shell',command='cat report.any',outputs=['report.any']),read,done];expected='FAILED'
    elif mode in ('cache_shell_branch','cache_git_tag','cache_shell_env'):
        probe=action('grep',path='cache-target',pattern='CACHE_PROOF')
        if mode=='cache_shell_env':
            change=action('shell',command='env python mutate.py')
        else:
            name='branch' if mode=='cache_shell_branch' else 'tag'
            change=action('shell',command='git branch review-proof') if name=='branch' else action('git',args='tag review-proof')
            probe=action('glob',path='.git',pattern='**/review-proof')
        replies=[probe,change,probe,done];expected='DONE'
    elif mode=='observation':acceptance={'requires_observation':True};replies=[done];expected='FAILED'
    elif mode=='mutation':acceptance={'requires_action':True};replies=[action('file_read',path='tiny'),done];expected='FAILED'
    elif mode=='failed_tool':replies=[action('file_read',path='missing'),done];expected='FAILED'
    else:raise ValueError(mode)
    class Provider(BaseHTTPRequestHandler):
        def log_message(self,*_):pass
        def do_POST(self):
            nonlocal calls
            self.rfile.read(int(self.headers['Content-Length'])) # Fixed responses, no prompt matching.
            reply=replies[min(calls,len(replies)-1)];calls+=1
            actions=reply if isinstance(reply,list) else [reply]
            message={'content':reply if isinstance(reply,str) else json.dumps(actions,ensure_ascii=False)}
            if protocol=='native' and not isinstance(reply,str):
                message={'content':'','tool_calls':[{'id':f'call-{calls}-{i}','type':'function','function':{'name':item['tool'],'arguments':json.dumps(item['args'])}} for i,item in enumerate(actions)]}
            respond_json(self,json.dumps({'choices':[{'message':message,'finish_reason':'stop'}]}).encode())
    server=ThreadingHTTPServer(('127.0.0.1',0),Provider)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='coa-contract-') as folder:
            work=Path(folder);(work/'tiny').write_text('TINY_SOURCE_PROOF',encoding='utf8');(work/'long.dat').write_text('x'*9001,encoding='utf8')
            if mode.startswith('cache_'):
                if mode=='cache_shell_env':
                    (work/'cache-target').write_text('before',encoding='utf8')
                    (work/'mutate.py').write_text("open('cache-target','w').write('CACHE_PROOF')",encoding='utf8')
                else:
                    subprocess.run(['git','init','-q',str(work)],check=True)
                    subprocess.run(['git','-C',str(work),'-c','user.name=Fixture','-c','user.email=fixture@example.invalid','-c','commit.gpgsign=false','commit','--allow-empty','-qm','fixture'],check=True)
            if mode in ('edit','edit_failed','edit_stale','preexisting','stale','script_unchanged','baseline_capacity'):(work/'report.any').write_text('OLD_ARTIFACT',encoding='utf8')
            if mode=='baseline_capacity':
                for path in sources:(work/path).write_text('SOURCE',encoding='utf8')
            (work/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'contract-fixture','llm.api_key':'fixture','llm.base_url':f'http://127.0.0.1:{server.server_port}','reasoning.max_rounds':16}))
            with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
            def api(path,body=None):
                req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
                with urllib.request.urlopen(req,timeout=5) as response:return json.load(response)
            with (BUILD/f'contract-{protocol}-{mode}.log').open('w',encoding='utf8') as log:
                proc=subprocess.Popen([str(BINARY),'--state',folder,'--workspace',folder,'serve',str(port)],cwd=ROOT,env={**os.environ,'COA_TOOL_PROTOCOL':protocol},stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                try:
                    deadline=time.monotonic()+25
                    for _ in range(150):
                        try:api('/v1/scheduler');break
                        except OSError:assert proc.poll() is None;time.sleep(.05)
                    body={'message':prompt,'session':'contract'}
                    if protocol=='native' and mode in ('commentary','quoted_commentary','stuck'):
                        body['acceptance']={'requires_observation':True}
                    if acceptance is not None:body['acceptance']=acceptance
                    tid=api('/v1/chat',body)['id']
                    while True:
                        task=api(f'/v1/tasks/{tid}')
                        if task['status'] not in ('RUNNING','QUEUED'):break
                        assert time.monotonic()<deadline,task;time.sleep(.02)
                    assert task['status']==expected,(protocol,mode,task)
                    assert calls<=(16 if mode=='baseline_capacity' else 10),(mode,calls,task)
                    if expected=='DONE':
                        assert task['output']==FINAL,task
                        if mode=='artifact':assert (work/'report.any').read_text()==CONTENT
                        if mode=='script_output':assert (work/'report.any').read_text()==CONTENT
                    else:assert not api('/v1/chat/history?session=contract')['turns']
                    if mode=='prefixed':assert task['tool_calls']==1 and task['model_failures']==0,task
                    if mode=='quoted_commentary':assert task['tool_calls']==1 and task['model_failures']==0,task
                    if mode=='commentary':assert task['tool_calls']==1 and task['model_failures']==0,task
                    if mode=='stuck':assert calls==3 and task['tool_calls']==0,task
                    if mode=='full':assert task['tool_calls']==3,task
                    if mode=='optional':assert task['tool_calls']==1,task
                    if mode.startswith('cache_'):
                        steps=task['steps']
                        assert len(steps)==3,steps
                        result=steps[-1]['out']
                        assert 'previous search result reused' not in result,steps
                        assert ('cache-target' if mode=='cache_shell_env' else 'review-proof') in result,steps
                    if mode in ('missing','erase'):assert task['tool_calls']==0 and not (work/'report.any').exists(),task
                    if mode=='script_reuse':
                        # Same process, same session/lane, same reasoning object:
                        # an earlier production receipt cannot satisfy a new run.
                        replies[:]=[declare(artifacts=['report.any']),action('shell',command='cat report.any',outputs=['report.any']),read,done]
                        calls=0
                        tid=api('/v1/chat',body)['id']
                        deadline=time.monotonic()+25
                        while True:
                            second=api(f'/v1/tasks/{tid}')
                            if second['status'] not in ('RUNNING','QUEUED'):break
                            assert time.monotonic()<deadline,second;time.sleep(.02)
                        assert second['status']=='FAILED',(protocol,'cross-run artifact accepted',second)
                        assert calls<=7,(calls,second)
                        print('PASS:',protocol,'same-process second run rejects old artifact',flush=True)
                    print('PASS #236:',protocol,mode,expected,'calls',calls,'tools',task['tool_calls'],flush=True)
                finally:proc.terminate();proc.wait(timeout=5)
    finally:server.shutdown();server.server_close()

if __name__=='__main__':
    for protocol in ('native','legacy'):
        for mode in ('edit','edit_failed','edit_stale'):exercise(protocol,mode)
    exercise('native','plain_final')
    exercise('legacy','prefixed')
    for mode in ('cache_shell_branch','cache_git_tag'):
        exercise('native',mode)
    for protocol in ('native','legacy'):exercise(protocol,'baseline_capacity')
    for protocol in ('native','legacy'):
        for mode in ('unreceipted_rewrite','late_output_declaration','nonproducing_output_declaration'):exercise(protocol,mode)
    if os.name!='nt':exercise('native','cache_shell_env')
    for protocol in ('legacy','native'):
        for mode in ('chat','commentary','quoted_commentary','stuck','artifact','missing','erase','preexisting','write_only','stale','script_output','script_reuse','script_unchanged','script_rewrite','full','partial','small','optional','api_missing','observation','mutation','failed_tool'):
            exercise(protocol,mode)
        for prompt in ('为什么帮我解释如何修改代码并完整读取文件，输出HTML报告。','Do not read. Read all source. I will write. Help install fix create.','Прочитайте исходники. 完整读文件只是引用。'):
            exercise(protocol,'chat',prompt)
