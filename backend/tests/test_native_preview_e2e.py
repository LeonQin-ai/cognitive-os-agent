"""True provider SSE -> Runtime provisional preview -> CLI, before completion."""
import json, os, socket, subprocess, tempfile, threading, time, urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
binary=build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
sent=threading.Event();release=threading.Event();mode='good';calls=[]
answer='真实流式中文😀 "引号"\n'+''.join(f'第{i:04d}段：Unicode 测试 {i*i:08d}，证据编号 {i*7919:09d}。\n' for i in range(2500))+'END_OF_STREAM'
class Provider(BaseHTTPRequestHandler):
    def log_message(self,*_):pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));calls.append(body)
        assert body['stream'] is True and body['tools']
        if mode=='auth':
            self.send_response(401);self.send_header('Content-Length','2');self.end_headers();self.wfile.write(b'{}');return
        self.send_response(200);self.send_header('Content-Type','text/event-stream');self.end_headers()
        def event(delta,finish=None):
            self.wfile.write(('data: '+json.dumps({'choices':[{'delta':delta,'finish_reason':finish}]})+'\n\n').encode());self.wfile.flush()
        event({'tool_calls':[{'index':0,'id':'test-id','function':{'name':'file_write' if mode=='tool-incomplete' else 'complete_task','arguments':''}}]})
        arguments=json.dumps({'status':'complete','requires_tools':False,'answer':answer})
        if mode=='good':
            event({'tool_calls':[{'index':0,'function':{'arguments':arguments[:400]}}]});sent.set();assert release.wait(15)
            for i in range(400,len(arguments),1703):event({'tool_calls':[{'index':0,'function':{'arguments':arguments[i:i+1703]}}]})
            event({},'tool_calls');self.wfile.write(b'data: [DONE]\n\n');self.wfile.flush()
        else:
            # A stream may expose partial text but never authorize incomplete calls.
            fragment='{"path":"should-not-exist.txt","content":"unfinished' if mode=='tool-incomplete' else arguments[:400]
            event({'tool_calls':[{'index':0,'function':{'arguments':fragment}}]})
            if mode in ('length','tool-incomplete'):event({},'length');self.wfile.write(b'data: [DONE]\n\n');self.wfile.flush()
provider=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=provider.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory(prefix='coa-stream-') as tmp:
    state=Path(tmp)/'state';state.mkdir()
    (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixture','llm.base_url':f'http://127.0.0.1:{provider.server_port}','llm.api_key':'fixture','reasoning.max_rounds':8}),encoding='utf-8')
    with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    base=f'http://127.0.0.1:{port}'
    def request(path,body=None):
        r=urllib.request.Request(base+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(r,timeout=5) as response:return json.load(response)
    with (build/'native-preview-e2e.log').open('w',encoding='utf-8') as log:
        server=subprocess.Popen([str(binary),'--state',str(state),'--workspace',tmp,'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            for _ in range(100):
                try:request('/v1/scheduler');break
                except OSError:assert server.poll() is None;time.sleep(.05)
            sid=request('/v1/chat/sessions',{})['id']
            proc=subprocess.Popen([str(binary),'chat','--connect',base,'--session',sid,'-p','Provide the answer'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=os.environ)
            assert sent.wait(10)
            deadline=time.monotonic()+5
            while True:
                task=request('/v1/tasks/0')
                if task.get('answer_preview'):break
                assert time.monotonic()<deadline,task;time.sleep(.03)
            assert task['status']=='RUNNING' and 'output' not in task and task['preview_provisional']
            assert answer.startswith(task['answer_preview']) and '😀' in task['answer_preview']
            assert proc.poll() is None,'client ended before provider completion'
            release.set();out,err=proc.communicate(timeout=25)
            assert proc.returncode==0,err.decode(errors='replace')
            assert out.decode('utf-8').replace('\r\n','\n')==answer+'\n','lost or duplicated stream text'
            final=request('/v1/tasks/0');assert final['status']=='DONE' and final['output']==answer
            for bad in ('length','disconnected','tool-incomplete','auth'):
                mode=bad
                tid=request('/v1/chat',{'session':sid,'message':'Another answer','stream':True})['id'];deadline=time.monotonic()+15
                while True:
                    task=request('/v1/tasks/'+str(tid))
                    if task['status'] not in ('QUEUED','RUNNING'):break
                    assert time.monotonic()<deadline;time.sleep(.03)
                assert task['status']=='FAILED',task
                assert not task.get('steps'),'partial native calls executed'
                assert not (Path(tmp)/'should-not-exist.txt').exists()
                if bad=='auth':assert task.get('model_failure',{}).get('http_status')==401,task
            assert len(calls)<30,'unbounded stream retries'
            print('PASS: actual SSE preview while RUNNING, split Unicode/escapes, >64KiB full CLI answer, no duplicates, incomplete/length streams cannot complete or execute tools')
        finally:release.set();server.terminate();server.wait(timeout=10)
finally:provider.shutdown();provider.server_close()
