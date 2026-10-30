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
    if mode=='chat':replies=[done];expected='DONE'
    elif mode=='commentary':replies=['Changing wording does not finish a task.','I will inspect the file.',action('file_read',path='tiny'),done];expected='DONE'
    elif mode=='stuck':replies=['准备继续。','The next phase is still pending.','还需要继续查看。'];expected='FAILED'
    elif mode in ('artifact','missing','erase','preexisting','write_only','stale'):
        replies=[declare(artifacts=['report.any'])]
        if mode in ('artifact','write_only','stale'):replies += [write]
        if mode in ('artifact','preexisting','stale'):replies += [read]
        if mode=='stale':replies += [action('file_write',path='report.any',content=CONTENT+' changed')]
        replies += [done];expected='DONE' if mode=='artifact' else 'FAILED'
    elif mode in ('full','partial','small','optional'):
        path='tiny' if mode=='small' else 'long.dat'
        if mode!='optional':acceptance={'full_reads':[path]}
        replies=[action('file_read',path=path)]
        if mode=='full':replies += [action('file_read',path=path,offset_bytes=4000),action('file_read',path=path,offset_bytes=8000)]
        replies += [done];expected='FAILED' if mode=='partial' else 'DONE'
    elif mode=='api_missing':acceptance={'artifacts':['report.any']};replies=[done];expected='FAILED'
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
            message={'content':reply if isinstance(reply,str) else json.dumps([reply],ensure_ascii=False)}
            if protocol=='native' and not isinstance(reply,str):
                message={'content':'','tool_calls':[{'id':f'call-{calls}','type':'function','function':{'name':reply['tool'],'arguments':json.dumps(reply['args'])}}]}
            respond_json(self,json.dumps({'choices':[{'message':message,'finish_reason':'stop'}]}).encode())
    server=ThreadingHTTPServer(('127.0.0.1',0),Provider)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='coa-contract-') as folder:
            work=Path(folder);(work/'tiny').write_text('TINY_SOURCE_PROOF',encoding='utf8');(work/'long.dat').write_text('x'*9001,encoding='utf8')
            if mode in ('preexisting','stale'):(work/'report.any').write_text('OLD_ARTIFACT',encoding='utf8')
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
                    if acceptance is not None:body['acceptance']=acceptance
                    tid=api('/v1/chat',body)['id']
                    while True:
                        task=api(f'/v1/tasks/{tid}')
                        if task['status'] not in ('RUNNING','QUEUED'):break
                        assert time.monotonic()<deadline,task;time.sleep(.02)
                    assert task['status']==expected,(protocol,mode,task)
                    assert calls<=10,(mode,calls,task)
                    if expected=='DONE':
                        assert task['output']==FINAL,task
                        if mode=='artifact':assert (work/'report.any').read_text()==CONTENT
                    else:assert not api('/v1/chat/history?session=contract')['turns']
                    if mode=='commentary':assert task['tool_calls']==1 and task['model_failures']==0,task
                    if mode=='stuck':assert calls==3 and task['tool_calls']==0,task
                    if mode=='full':assert task['tool_calls']==3,task
                    if mode=='optional':assert task['tool_calls']==1,task
                    if mode in ('missing','erase'):assert task['tool_calls']==0 and not (work/'report.any').exists(),task
                    print('PASS #236:',protocol,mode,expected,'calls',calls,'tools',task['tool_calls'],flush=True)
                finally:proc.terminate();proc.wait(timeout=5)
    finally:server.shutdown();server.server_close()

if __name__=='__main__':
    for protocol in ('legacy','native'):
        for mode in ('chat','commentary','stuck','artifact','missing','erase','preexisting','write_only','stale','full','partial','small','optional','api_missing','observation','mutation','failed_tool'):
            exercise(protocol,mode)
        for prompt in ('为什么帮我解释如何修改代码并完整读取文件，输出HTML报告。','Do not read. Read all source. I will write. Help install fix create.','Прочитайте исходники. 完整读文件只是引用。'):
            exercise(protocol,'chat',prompt)
