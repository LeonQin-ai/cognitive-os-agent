"""Actionable incomplete diagnostics and real task resume regression (#128)."""
import json, socket, subprocess, tempfile, threading, time, urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
counts={}
class Provider(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text=body['messages'][-1]['content']
        current=text.split('## Current request\n',1)[-1].split('\n## ',1)[0]
        if 'UTF8_DIAGNOSTIC' in current:
            n=counts.get('utf8',0); counts['utf8']=n+1
            answer=(json.dumps([{'tool':'shell','args':{'command':'python emit_invalid.py'}}]) if n==0 else
                    json.dumps([{'tool':'shell','args':{'command':'python emit_valid.py'}}]) if n==1 else
                    '编码记录已核对，采样完成。')
        elif 'REGISTER_DIAGNOSTIC' in current: answer='技能已经注册。'
        else:
            n=counts.get('read',0); counts['read']=n+1
            if n==0: answer=json.dumps([{'tool':'file_read','args':{'path':'partial.txt'}}])
            elif counts.get('recover') and n==1:
                answer=json.dumps([{'tool':'file_read','args':{'path':'partial.txt','offset_bytes':4000}}])
            elif counts.get('recover'):
                assert 'EOF_DIAGNOSTIC' in text; answer='完整读取完成，已核对 EOF_DIAGNOSTIC。'
            else: answer='全部阅读完成。'
        data=json.dumps({'choices':[{'message':{'content':answer}}]}).encode()
        self.send_response(200); self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
backend = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coa-completion-recovery-') as folder:
    state = Path(folder)
    provider = ThreadingHTTPServer(('127.0.0.1',0), Provider)
    threading.Thread(target=provider.serve_forever,daemon=True).start()
    (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'skill-test',
        'llm.base_url':f'http://127.0.0.1:{provider.server_port}','reasoning.max_rounds':6}),encoding='utf-8')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0)); port = sock.getsockname()[1]
    def request(path, body=None):
        req = urllib.request.Request(f'http://127.0.0.1:{port}'+path,
            data=json.dumps(body).encode() if body is not None else None,
            headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(req,timeout=5) as r: return json.load(r)
    with (backend/'build/completion-diagnostics-e2e.log').open('w',encoding='utf-8') as log:
        process = subprocess.Popen([str(backend/'build/cognitive-os-agent.exe'),'--state',str(state),
            '--workspace',str(state),'serve',str(port)],cwd=backend,stdout=log,stderr=log,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            deadline = time.monotonic()+20
            while True:
                try: request('/v1/scheduler'); break
                except OSError:
                    assert process.poll() is None and time.monotonic()<deadline
                    time.sleep(.1)
            def finished(task_id):
                deadline=time.monotonic()+20
                while True:
                    t=request('/v1/tasks/'+str(task_id))
                    if t['status'] in ('DONE','FAILED','CANCELLED','TIMEOUT'): return t
                    assert time.monotonic()<deadline,t
                    time.sleep(.05)
            no_action=finished(request('/v1/tasks',{'prompt':'注册这些skills REGISTER_DIAGNOSTIC'})['id'])
            assert no_action['status']=='FAILED' and 'skill_manage' in no_action['output'],no_action
            (state/'partial.txt').write_text('x'*7500+'EOF_DIAGNOSTIC',encoding='utf-8')
            started=request('/v1/tasks',{'prompt':'完整读取文件 partial.txt READ_DIAGNOSTIC'})
            partial=finished(started['id'])
            assert partial['status']=='FAILED' and 'offset_bytes=4000' in partial['output'] and 'partial.txt' in partial['output'],partial
            counts['recover']=True; counts['read']=0
            resumed=finished(request('/v1/tasks/'+str(started['id'])+'/resume',{})['id'])
            assert resumed['status']=='DONE' and 'EOF_DIAGNOSTIC' in resumed['output'],resumed
            (state/'emit_invalid.py').write_text('import sys\nsys.stdout.buffer.write(("NORMAL_UNICODE_正常中文"+"中"*1000+"EOF_UTF8_DIAGNOSTIC").encode("utf-8"))\nsys.exit(1)\n',encoding='utf-8')
            (state/'emit_valid.py').write_text('import sys\nsys.stdout.buffer.write("NORMAL_UNICODE_正常中文".encode("utf-8"))\n',encoding='utf-8')
            encoded=finished(request('/v1/tasks',{'prompt':'检查工具输出编码 UTF8_DIAGNOSTIC'})['id'])
            assert encoded['status']=='DONE',encoded
            steps=encoded.get('steps',[])
            assert steps and 'NORMAL_UNICODE_正常中文' in steps[0].get('out','') and 'EOF_UTF8_DIAGNOSTIC' in steps[0].get('out',''),steps
            assert counts.get('utf8')==3,counts
            print('PASS: invalid UTF-8 tool output remains valid task/progress JSON; valid Chinese survives')
            print('PASS: missing registration operation and unread file have actionable causes; resumed read reaches EOF and DONE')
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
            provider.shutdown()
