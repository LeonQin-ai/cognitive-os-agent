"""Actual Runtime: immutable per-task permission modes and one-shot approvals."""
import json, os, socket, subprocess, tempfile, threading, time, urllib.request, urllib.error
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
class Model(BaseHTTPRequestHandler):
    calls=0
    def log_message(self,*args):pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        seen=Model.calls>0;Model.calls+=1
        name,args=('complete_task',{'status':'complete','requires_tools':False,'answer':'Approved operation verified.'}) if seen else ('file_write',{'path':'proof.txt','content':'approved'})
        raw=json.dumps({'choices':[{'message':{'content':'','tool_calls':[{'id':'fixture','type':'function','function':{'name':name,'arguments':json.dumps(args)}}]},'finish_reason':'tool_calls'}]}).encode()
        self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
provider=ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=provider.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory(prefix='coa-approval-') as folder:
    state=Path(folder)
    (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixture','llm.api_key':'fixture','llm.base_url':f'http://127.0.0.1:{provider.server_port}','reasoning.max_rounds':3}),encoding='utf8')
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    def req(method,path,body=None,expected=200):
        r=urllib.request.Request(f'http://127.0.0.1:{port}'+path,method=method,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
        try:response=urllib.request.urlopen(r,timeout=5)
        except urllib.error.HTTPError as e:response=e
        with response:obj=json.load(response);assert response.code==expected,(response.code,obj);return obj
    def wait(tid,predicate):
        deadline=time.monotonic()+10
        while True:
            t=req('GET',f'/v1/tasks/{tid}')
            if predicate(t):return t
            assert time.monotonic()<deadline,t
            time.sleep(.03)
    with (build/'approval-e2e.log').open('w',encoding='utf8') as log:
        server=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',folder,'--workspace',folder,'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            for _ in range(100):
                try:req('GET','/v1/scheduler');break
                except OSError:assert server.poll() is None;time.sleep(.05)
            req('POST','/v1/chat',{'session':'s','message':'write proof','permission_mode':'invalid'},400)
            req('POST','/v1/chat',{'session':'s','message':'write proof','permission_mode':5},400)
            for mode in ('ask','auto'):
                Model.calls=0
                tid=req('POST','/v1/chat',{'session':mode,'message':'write proof','permission_mode':mode})['id']
                t=wait(tid,lambda t:'approval' in t);assert not (state/'proof.txt').exists()
                body={'session':mode,'id':t['approval']['id'],'allow':True}
                req('POST',f'/v1/tasks/{tid}/approval',{**body,'session':'other'},409)
                req('POST',f'/v1/tasks/{tid}/approval',{**body,'id':999},409)
                req('POST',f'/v1/tasks/{tid}/approval',body)
                req('POST',f'/v1/tasks/{tid}/approval',body,409)
                assert wait(tid,lambda t:t['status'] in ('DONE','FAILED'))['status']=='DONE'
                assert (state/'proof.txt').read_text()=='approved';(state/'proof.txt').unlink()
                if mode=='ask':
                    ask_task_id=tid
                    Model.calls=0
                    resumed=req('POST',f'/v1/tasks/{tid}/resume',{})['id']
                    restored=wait(resumed,lambda t:'approval' in t)
                    assert restored['permission_mode']==1 and not (state/'proof.txt').exists()
                    req('DELETE',f'/v1/tasks/{resumed}');wait(resumed,lambda t:t['status']=='CANCELLED')
            Model.calls=0
            tid=req('POST','/v1/chat',{'session':'cancel','message':'write proof','permission_mode':'ask'})['id']
            wait(tid,lambda t:'approval' in t);req('DELETE',f'/v1/tasks/{tid}')
            assert wait(tid,lambda t:t['status']=='CANCELLED')['status']=='CANCELLED';assert not (state/'proof.txt').exists()
            Model.calls=0
            tid=req('POST','/v1/chat',{'session':'full','message':'write proof','permission_mode':'full'})['id']
            assert wait(tid,lambda t:t['status'] in ('DONE','FAILED'))['status']=='DONE'
            assert (state/'proof.txt').read_text()=='approved'
            server.terminate();server.wait(timeout=5);(state/'proof.txt').unlink()
            with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
            server=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',folder,'--workspace',folder,'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            for _ in range(100):
                try:req('GET','/v1/scheduler');break
                except OSError:assert server.poll() is None;time.sleep(.05)
            Model.calls=0
            restored_id=req('POST',f'/v1/tasks/{ask_task_id}/resume',{})['id']
            restored=wait(restored_id,lambda t:'approval' in t)
            assert restored['permission_mode']==1 and not (state/'proof.txt').exists()
            req('DELETE',f'/v1/tasks/{restored_id}');wait(restored_id,lambda t:t['status']=='CANCELLED')
            Model.calls=0
            full_resume=req('POST',f'/v1/tasks/{tid}/resume',{})['id']
            assert wait(full_resume,lambda t:t['status'] in ('DONE','FAILED'))['status']=='DONE'
            assert (state/'proof.txt').read_text()=='approved'
        finally:server.terminate();server.wait(timeout=5)
finally:provider.shutdown();provider.server_close()
print('PASS: ask/auto block writes until one-shot approval; session/stale checks; cancellation; full access; invalid mode')
