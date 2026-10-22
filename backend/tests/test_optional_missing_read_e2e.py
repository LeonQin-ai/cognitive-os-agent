"""Structured optional negative evidence; never waive unrelated or mutating failures."""
import json, os, socket, subprocess, tempfile, threading, time, urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
root=Path(__file__).resolve().parents[1]
build=root/os.environ.get('COA_BUILD_DIR','build')
binary=build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
class Provider(BaseHTTPRequestHandler):
    calls=0; mode=''; workspace=None
    def log_message(self,*args): pass
    def do_POST(self):
        self.rfile.read(int(self.headers['Content-Length'])); n=Provider.calls;Provider.calls+=1
        mode=Provider.mode; missing=str(Provider.workspace/'absent.ts')
        if n==0:
            call=('list_dir',{'path':str(Provider.workspace)}) if mode=='early-evidence' else ('file_write' if mode=='mutation' else 'file_read',{'path':str(Provider.workspace/'other') if mode=='mutation' else missing,'content':'data'} if mode=='mutation' else {'path':missing})
        elif n==1:
            call=('file_read',{'path':missing}) if mode=='early-evidence' else ('file_write',{'path':missing,'content':'now exists'}) if mode=='exists' else ('list_dir',{'path':str(Provider.workspace/'other') if mode=='wrong-parent' else str(Provider.workspace)})
        else:
            answer='\n'.join(f'源代码观察 {i}: result_{i*i+7}' for i in range(500))+'\nANSWER_END'
            args={'status':'complete','requires_tools':False,'answer':answer}
            if mode!='no-decision':args['unavailable_optional_reads']=[{'path':str(Provider.workspace/'other') if mode=='mutation' else missing,'reason':'Optional guessed source; analysis uses observed source instead.','evidence_step_id':1 if mode=='early-evidence' else 999 if mode=='bad-id' else 2}]
            call=('complete_task',args)
        name,args=call
        body={'choices':[{'message':{'content':'','tool_calls':[{'id':'call','type':'function','function':{'name':name,'arguments':json.dumps(args)}}]},'finish_reason':'tool_calls'}]}
        raw=json.dumps(body).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
provider=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=provider.serve_forever,daemon=True).start()
try:
    for mode in ('verified','no-decision','wrong-parent','early-evidence','bad-id','mutation','exists'):
        Provider.calls=0;Provider.mode=mode
        with tempfile.TemporaryDirectory(prefix='coa-negative-read-') as directory:
            work=Path(directory);Provider.workspace=work;(work/'other').mkdir()
            state=work/'state';state.mkdir();(state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixture','llm.base_url':f'http://127.0.0.1:{provider.server_port}','llm.api_key':'fixture','reasoning.max_rounds':10}),encoding='utf-8')
            def port():
                with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
            current=port()
            def request(path,body=None):
                req=urllib.request.Request(f'http://127.0.0.1:{current}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
                with urllib.request.urlopen(req,timeout=5) as response:return json.load(response)
            with (build/f'optional-read-{mode}.log').open('w',encoding='utf-8') as log:
                def start():
                    p=subprocess.Popen([str(binary),'--state',str(state),'--workspace',str(work),'serve',str(current)],cwd=root,stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                    for _ in range(100):
                        try:request('/v1/scheduler');return p
                        except OSError:assert p.poll() is None;time.sleep(.05)
                    raise AssertionError('startup')
                p=start()
                try:
                    task=request('/v1/chat',{'session':'negative-read','message':'Analyze sources; the guessed absent.ts file is optional. Verify absence and finish with evidence.'});deadline=time.monotonic()+25
                    while True:
                        task=request('/v1/tasks/'+str(task['id']))
                        if task['status'] not in ('RUNNING','QUEUED'):break
                        assert time.monotonic()<deadline;time.sleep(.02)
                    assert task['status']==('DONE' if mode=='verified' else 'FAILED'),(mode,task['status'])
                    assert Provider.calls<=9,(mode,Provider.calls)
                    if mode=='verified':
                        assert task['output'].endswith('ANSWER_END') and len(task['output'].encode())>4095
                        expected=task['output'];deadline=time.monotonic()+3
                        while not any(row.get('output')==expected for row in request('/v1/tasks/journal')):
                            assert time.monotonic()<deadline;time.sleep(.02)
                finally:p.terminate();p.wait(timeout=5)
                if mode=='verified':
                    current=port();p=start()
                    try:assert any(row.get('output')==expected for row in request('/v1/tasks/journal'))
                    finally:p.terminate();p.wait(timeout=5)
            print('PASS:',mode,flush=True)
finally:provider.shutdown();provider.server_close()
