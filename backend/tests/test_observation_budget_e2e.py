"""Model-sized observation retention through real CLI and native HTTP tools."""
import json, os, secrets, socket, subprocess, tempfile, threading, time, urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
backend=Path(__file__).resolve().parents[1]
build=backend/os.environ.get('COA_BUILD_DIR','build')
for provider in ('openai','anthropic'):
 for window in (65536,1048576):
    count=0; errors=[]; marker=secrets.token_hex(20)
    class Provider(BaseHTTPRequestHandler):
        def log_message(self,*args):pass
        def do_POST(self):
            global count
            try:
                body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                n=count;count+=1;assert n<35
                assert body.get('tools')
                message_bytes=sum(len(str(m.get('content','')).encode()) for m in body['messages'])
                assert message_bytes+len(json.dumps(body['tools']).encode())+body['max_tokens']*2<=window*2, (window,message_bytes)
                if n<30:calls=[('file_read',{'path':f'part-{n}.txt','max_bytes':4000})]
                else:
                    prompt=body['messages'][-1]['content']
                    assert (marker in prompt)==(window>65536), (provider,window,'early evidence retention wrong')
                    calls=[('complete_task',{'status':'complete','requires_tools':False,'answer':'Evidence collected and analyzed.'})]
                if provider=='openai':
                    response={'choices':[{'message':{'content':'','tool_calls':[{'id':'c','type':'function','function':{'name':name,'arguments':json.dumps(args)}} for name,args in calls]},'finish_reason':'tool_calls'}]}
                else:response={'content':[{'type':'tool_use','id':'c','name':name,'input':args} for name,args in calls],'stop_reason':'tool_use'}
                raw=json.dumps(response).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
            except Exception as error:errors.append(repr(error));self.send_error(500)
    with tempfile.TemporaryDirectory(prefix='coa-observation-budget-') as folder:
        state=Path(folder);server=ThreadingHTTPServer(('127.0.0.1',0),Provider)
        threading.Thread(target=server.serve_forever,daemon=True).start()
        for i in range(30):
            text='\n'.join(secrets.token_hex(45) for _ in range(40))
            if i==0:text=marker+'\n'+text
            (state/f'part-{i}.txt').write_text(text,encoding='utf-8')
        route={'name':'fixture','provider':provider,'model':'window-fixture','api_key':'local-fixture','base_url':f'http://127.0.0.1:{server.server_port}','context_window':window,'max_output_tokens':65536,'context_source':'provider_metadata'}
        (state/'routes.json').write_text(json.dumps([route]),encoding='utf-8')
        (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.'+k:v for k,v in route.items() if k in ('provider','model','base_url','api_key')}|{'reasoning.max_rounds':-1}),encoding='utf-8')
        with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        def request(path,body=None):
            req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
            with urllib.request.urlopen(req,timeout=5) as response:return json.load(response)
        with (build/f'observation-budget-{provider}-{window}.log').open('w',encoding='utf-8') as log:
            process=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',folder,'--workspace',folder,'serve',str(port)],cwd=backend,stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            try:
                for _ in range(100):
                    try:request('/v1/scheduler');break
                    except OSError:assert process.poll() is None;time.sleep(.1)
                task=request('/v1/chat',{'message':'Collect the supplied parts and deliver the analysis.','session':'retention'});deadline=time.monotonic()+40
                while True:
                    task=request('/v1/tasks/'+str(task['id']))
                    if task['status'] not in ('RUNNING','QUEUED','CANCELLING'):break
                    assert time.monotonic()<deadline;time.sleep(.03)
                assert not errors, errors
                assert task['status']=='DONE' and task['tool_calls']==30,(task['status'],task['tool_calls'])
                print('PASS:',provider,window,'30 reads; correct retention and bounded request',flush=True)
            finally:
                process.terminate()
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait()
                server.shutdown();server.server_close()
