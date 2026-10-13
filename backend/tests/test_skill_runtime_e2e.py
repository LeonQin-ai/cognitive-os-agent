"""Real agent skill discovery/registration/instruction handoff regression (#124/#126/#127)."""
import json, socket, subprocess, tempfile, threading, time, urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
counts = {}; errors = []
class Provider(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text = body['messages'][-1]['content']
        n = counts.get('calls', 0); counts['calls'] = n + 1
        try:
            assert 'live-skill' in body['messages'][0]['content'], 'copied skill not visible in planner'
            if n == 0:
                answer = json.dumps([{'tool':'skill_manage','args':{'names':['live-skill','second-skill']}}])
            elif n == 1:
                assert 'Registered and persisted' in text
                answer = json.dumps([{'tool':'skill','args':{'name':'live-skill','args':{'path':'skill-result.txt'}}}])
            elif n == 2:
                assert '[prompt skill instructions]' in text and 'skill-result.txt' in text
                answer = json.dumps([{'tool':'file_write','args':{'path':'skill-result.txt','content':'SKILL_EXECUTED'}}])
            else: answer = '文件已生成，两个技能已注册。'
        except Exception as e:
            errors.append(str(e)); answer = '测试未完成。'
        data = json.dumps({'choices':[{'message':{'content':answer}}]}).encode()
        self.send_response(200); self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
backend = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coa-skill-runtime-') as folder:
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
    with (backend/'build/skill-runtime-e2e.log').open('w',encoding='utf-8') as log:
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
            # Copy AFTER startup: no manual reload endpoint is called.
            for name in ('live-skill','second-skill'):
                path = state/'skills'/name; path.mkdir(parents=True)
                (path/'SKILL.md').write_text('Create {{path}} with content SKILL_EXECUTED.',encoding='utf-8')
            catalogue = request('/v1/skills')
            assert 'live-skill' in json.dumps(catalogue), catalogue
            started = request('/v1/tasks',{'prompt':'创建 skill-result.txt，先注册 live-skill 和 second-skill，再按 live-skill 指令执行。'})
            deadline = time.monotonic()+25
            while True:
                task = request('/v1/tasks/'+str(started['id']))
                if task['status'] in ('DONE','FAILED','CANCELLED','TIMEOUT'): break
                assert time.monotonic()<deadline, task
                time.sleep(.05)
            assert task['status']=='DONE', task
            assert not errors, errors
            assert (state/'skill-result.txt').read_text()=='SKILL_EXECUTED'
            persisted = json.loads((state/'skills.json').read_text())
            assert {'live-skill','second-skill'} <= {x['name'] for x in persisted}
            assert counts['calls']==4, counts
            print('PASS: copy after startup -> automatic catalogue -> direct registration -> prompt instructions -> real file -> DONE')
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
            provider.shutdown()
