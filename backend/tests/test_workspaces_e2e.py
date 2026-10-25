"""Real Agent HTTP/tools: persistent workspaces and concurrent immutable cwd."""
import json, os, re, socket, subprocess, tempfile, threading, time, urllib.request, urllib.error
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
entered=threading.Event();release=threading.Event();counts={}
class Model(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text='\n'.join(m['content'] for m in body['messages'] if isinstance(m.get('content'),str))
        found=re.findall(r'## Execution workspace\n([^\n]+)',text);assert found,text[-1000:]
        directory=found[-1];n=counts.get(directory,0);counts[directory]=n+1
        if directory.replace('\\','/').endswith('项目 A') and n==0:entered.set();assert release.wait(10)
        cwd_args={'command':'[Console]::OutputEncoding=[Text.Encoding]::UTF8;$PWD.Path','shell':'powershell'} if os.name=='nt' else {'command':'pwd'}
        name,args=[('file_read',{'path':'marker.txt'}),('shell',cwd_args),('file_write',{'path':'result.txt','content':directory})][n] if n<3 else ('complete_task',{'status':'complete','requires_tools':False,'answer':'Verified workspace '+directory})
        raw=json.dumps({'choices':[{'message':{'content':'','tool_calls':[{'id':'c','type':'function','function':{'name':name,'arguments':json.dumps(args)}}]},'finish_reason':'tool_calls'}]}).encode()
        self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
model=ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=model.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory(prefix='coa-workspaces-') as tmp:
    base=Path(tmp).resolve();state=base/'state';state.mkdir();a=base/'项目 A';b=base/'project B';a.mkdir();b.mkdir()
    (a/'marker.txt').write_text('PROOF_A',encoding='utf-8');(b/'marker.txt').write_text('PROOF_B',encoding='utf-8')
    (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixture','llm.base_url':f'http://127.0.0.1:{model.server_port}','llm.api_key':'fixture','reasoning.max_rounds':12,'scheduler.workers':2}),encoding='utf-8')
    def new_port():
        with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]
    port=new_port()
    def req(path,body=None):
        r=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(r,timeout=15) as response:return json.load(response)
    with (build/'workspaces-e2e.log').open('w',encoding='utf-8') as log:
        def start():
            p=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',str(state),'--workspace',str(base),'serve',str(port)],cwd=root,stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            for _ in range(100):
                try:req('/v1/scheduler');return p
                except OSError:assert p.poll() is None;time.sleep(.05)
            raise AssertionError('startup')
        p=start()
        try:
            for path in (a,b):req('/v1/workspaces',{'path':str(path)})
            assert len(req('/v1/workspaces')['workspaces'])==2
            original=req('/v1/workspaces')['workspaces']
            assert all(w['created_ms']>0 for w in original)
            req('/v1/workspaces',{'path':str(a),'name':'renamed'})
            assert req('/v1/workspaces')['workspaces'][0]['created_ms']==original[0]['created_ms'],'rename preserves creation time'
            for path in (base/'missing',base/'marker-file'):
                try:req('/v1/workspaces',{'path':str(path)});raise AssertionError('invalid path accepted')
                except urllib.error.HTTPError as error:assert error.code==400
            sa=req('/v1/chat/sessions',{'workspace':str(a)})['id'];sb=req('/v1/chat/sessions',{'workspace':str(b)})['id']
            req('/v1/chat/sessions/'+sa+'/title',{'title':'自定义标题 <safe>'})
            assert next(s for s in req('/v1/chat/sessions') if s['id']==sa)['title']=='自定义标题 <safe>'
            try:req('/v1/chat/sessions/'+sa+'/title',{'title':'中'*43});raise AssertionError('oversize title accepted')
            except urllib.error.HTTPError as error:assert error.code==400
            ta=req('/v1/chat',{'message':'Inspect workspace A and write result.txt','session':sa})['id'];assert entered.wait(10)
            req('/v1/chat/sessions/'+sa+'/workspace',{'workspace':str(b)})
            tb=req('/v1/chat',{'message':'Inspect workspace B and write result.txt','session':sb})['id'];release.set()
            for tid,path,marker in ((ta,a,'PROOF_A'),(tb,b,'PROOF_B')):
                deadline=time.monotonic()+20
                while True:
                    t=req('/v1/tasks/'+str(tid))
                    if t['status'] not in ('RUNNING','QUEUED'):break
                    assert time.monotonic()<deadline;time.sleep(.03)
                if t['status']!='DONE':
                    (build/'workspaces-e2e-failure.json').write_text(json.dumps(t,ensure_ascii=False,indent=2),encoding='utf-8')
                assert t['status']=='DONE',t['output'];assert Path(t['workspace'])==path
                assert any(marker in s['out'] for s in t['steps'])
                cwd=next(s['out'] for s in t['steps'] if s['tool']=='shell');assert str(path).replace('\\','/').lower() in cwd.replace('\\','/').lower(),(str(path),cwd)
                assert Path((path/'result.txt').read_text(encoding='utf-8'))==path
            assert not (base/'result.txt').exists()
            resumed=req('/v1/tasks/'+str(ta)+'/resume',{})['id'];assert Path(req('/v1/tasks/'+str(resumed))['workspace'])==a
        finally:release.set();p.terminate();p.wait(timeout=5)
        port=new_port();p=start()
        try:
            assert req('/v1/workspaces')['workspaces']==[{**original[0],'name':'renamed'},original[1]],'creation metadata survives restart'
            assert next(s for s in req('/v1/chat/sessions') if s['id']==sa)['title']=='自定义标题 <safe>'
            assert len(req('/v1/workspaces')['workspaces'])==2
            original=req('/v1/workspaces')['workspaces']
            assert all(w['created_ms']>0 for w in original)
            req('/v1/workspaces',{'path':str(a),'name':'renamed'})
            assert req('/v1/workspaces')['workspaces'][0]['created_ms']==original[0]['created_ms'],'rename preserves creation time'
            sessions=req('/v1/chat/sessions');assert Path(next(s for s in sessions if s['id']==sa)['workspace'])==b
            client=subprocess.run([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),
                'chat','--no-stream','--connect',f'http://127.0.0.1:{port}','--session',sa,'-p','Report the workspace verification result'],
                capture_output=True,text=True,encoding='utf-8',timeout=20)
            assert client.returncode==0,client.stderr
            assert 'Verified workspace' in client.stdout and str(b).replace('\\','/') in client.stdout.replace('\\','/'),client.stdout
        finally:p.terminate();p.wait(timeout=5)
    print('PASS: workspace registration, invalid paths, concurrent relative reads/writes/cwd, immutable selection, resume and restart')
finally:
    # A stuck fixture teardown must fail with Python thread evidence, rather
    # than consume the entire platform job after printing PASS.
    def stop_model():
        model.shutdown()
        model.server_close()
    cleanup=threading.Thread(target=stop_model,daemon=True)
    cleanup.start();cleanup.join(5)
    if cleanup.is_alive():
        import faulthandler
        faulthandler.dump_traceback()
        raise AssertionError('Workspace provider fixture teardown exceeded 5 seconds')
