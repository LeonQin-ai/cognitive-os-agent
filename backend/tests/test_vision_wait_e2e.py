"""#131: real upload / image payload / slow provider / subsequent recovery."""
import base64, json, os, socket, subprocess, tempfile, threading, time
import urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

backend=Path(__file__).resolve().parents[1]
captures=[]
class Provider(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        captures.append(body)
        images=[p for m in body['messages'] if isinstance(m['content'],list)
                for p in m['content'] if p.get('type')=='image_url']
        if images:
            assert images[0]['image_url']['url'].startswith('data:image/png;base64,')
            assert body['max_tokens']==8192
            if len(captures)==1: time.sleep(8)
        else: assert body['max_tokens']==32768
        data=json.dumps({'choices':[{'message':{'content':'VISION_WAIT_OK'}}]}).encode()
        self.send_response(200); self.send_header('Content-Length',str(len(data))); self.end_headers()
        try: self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError): pass

with tempfile.TemporaryDirectory(prefix='coa-vision-wait-') as folder:
    state=Path(folder)
    provider=ThreadingHTTPServer(('127.0.0.1',0),Provider)
    threading.Thread(target=provider.serve_forever,daemon=True).start()
    (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'capture-vision',
        'llm.base_url':f'http://127.0.0.1:{provider.server_port}','llm.api_key':'local-fixture'}))
    with socket.socket() as sock: sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
    def request(path,body=None):
        data=json.dumps(body).encode() if isinstance(body,dict) else body
        with urllib.request.urlopen(urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=data,
            headers={'Content-Type':'application/json'}),timeout=5) as response: return json.load(response)
    binary=backend/os.environ.get('COA_BUILD_DIR','build')/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
    with (binary.parent/'vision-wait-e2e.log').open('w',encoding='utf-8') as log:
        env=os.environ.copy(); env['COA_LLM_VISION_TIMEOUT_MS']='1000'
        process=subprocess.Popen([str(binary),'--state',str(state),'--workspace',str(state),'serve',str(port)],
            cwd=backend,env=env,stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            deadline=time.monotonic()+20
            while True:
                try: request('/v1/scheduler'); break
                except OSError:
                    assert process.poll() is None and time.monotonic()<deadline
                    time.sleep(.05)
            png=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=')
            uploaded=request('/v1/upload?name=image.png&session=vision',png)
            def chat(attachments):
                before=time.monotonic()
                task_id=request('/v1/chat',{'message':'描述附件','session':'vision','attachments':attachments})['id']
                while True:
                    task=request('/v1/tasks/'+str(task_id))
                    if task['status'] in ('DONE','FAILED','TIMEOUT','CANCELLED'): return task,time.monotonic()-before
                    assert time.monotonic()-before<5,task
                    time.sleep(.02)
            failed,elapsed=chat([uploaded['id']])
            assert failed['status']=='FAILED' and 'capture-vision' in failed['output'] and '1000 ms' in failed['output'],failed
            assert elapsed<5,elapsed
            done,_=chat([uploaded['id']]); assert done['status']=='DONE',done
            plain,_=chat([]); assert plain['status']=='DONE',plain
            assert len(captures)==3,len(captures)
            print('PASS: slow image request fails within bound without retry loop; image retry and text succeed; payload and budgets verified')
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
            provider.shutdown()
