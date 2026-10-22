"""Browser MCP output preservation through real HTTP/scheduler/planner/file tools."""
import json, socket, subprocess, tempfile, threading, time, urllib.request, re
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from fixture_completion import reviewed_answer
counts={}; errors=[]; artifact=None
page=bytearray(b'x'*18000)
page[9000:9000+len(b'MIDDLE_BROWSER_PROOF')]=b'MIDDLE_BROWSER_PROOF'
page[17950:17950+len(b'END_BROWSER_PROOF')]=b'END_BROWSER_PROOF'
PAGE=page.decode()
class Provider(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_POST(self):
        global artifact
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path=='/mcp':
            method=body.get('method')
            if 'id' not in body:
                self.send_response(204); self.end_headers(); return
            if method=='initialize': result={'protocolVersion':'2024-11-05','capabilities':{'tools':{}},'serverInfo':{'name':'browser-fixture','version':'1'}}
            elif method=='tools/list': result={'tools':[{'name':'browser_evaluate','description':'Return the loaded article DOM text','inputSchema':{'type':'object','properties':{'function':{'type':'string'}},'required':['function']}}]}
            elif method=='tools/call':
                counts['browser_calls']=counts.get('browser_calls',0)+1
                assert 'document.body.innerText' in body['params']['arguments']['function']
                result={'content':[{'type':'text','text':PAGE}],'isError':False}
            else: result={}
            data=json.dumps({'jsonrpc':'2.0','id':body['id'],'result':result}).encode()
        else:
            text=body['messages'][-1]['content']; n=counts.get('calls',0); counts['calls']=n+1
            try:
                if n==0:
                    answer=json.dumps([{'tool':'mcp','args':{'server':'browser','tool':'browser_evaluate','args':{'function':'() => document.body.innerText'}}}])
                elif n<=5:
                    if artifact is None:
                        match=re.search(r'\{"path":.*?\}',text)
                        assert match, 'full result file reference missing'
                        artifact=json.loads(match.group())['path']
                    offset=(n-1)*4000; counts.setdefault('pages',[]).append(offset)
                    answer=json.dumps([{'tool':'file_read','args':{'path':artifact,'offset_bytes':offset}}])
                else:
                    assert 'MIDDLE_BROWSER_PROOF' in text and 'END_BROWSER_PROOF' in text
                    assert 'output elided' not in text
                    answer='页面完整读取，已核对 MIDDLE_BROWSER_PROOF 和 END_BROWSER_PROOF。'
            except Exception as e:
                errors.append(str(e)); answer='浏览器测试未完成。'
            answer=reviewed_answer(body,answer)
            data=json.dumps({'choices':[{'message':{'content':answer}}]}).encode()
        self.send_response(200); self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
backend = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coa-browser-results-') as folder:
    state = Path(folder)
    provider = ThreadingHTTPServer(('127.0.0.1',0), Provider)
    threading.Thread(target=provider.serve_forever,daemon=True).start()
    (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'skill-test',
        'llm.base_url':f'http://127.0.0.1:{provider.server_port}','reasoning.max_rounds':10}),encoding='utf-8')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0)); port = sock.getsockname()[1]
    def request(path, body=None):
        req = urllib.request.Request(f'http://127.0.0.1:{port}'+path,
            data=json.dumps(body).encode() if body is not None else None,
            headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(req,timeout=5) as r: return json.load(r)
    with (backend/'build/browser-results-e2e.log').open('w',encoding='utf-8') as log:
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
            request('/v1/mcp',{'name':'browser','transport':'http','url':f'http://127.0.0.1:{provider.server_port}/mcp'})
            started=request('/v1/tasks',{'prompt':'完整读取浏览器页面的全部内容 BROWSER_FULL_PAGE_TEST，包含正文中间和结尾。'})
            deadline=time.monotonic()+30
            while True:
                task=request('/v1/tasks/'+str(started['id']))
                if task['status'] in ('DONE','FAILED','CANCELLED','TIMEOUT'): break
                assert time.monotonic()<deadline,task
                time.sleep(.05)
            assert task['status']=='DONE' and not errors,(task,errors)
            assert counts.get('browser_calls')==1,counts
            assert 'MIDDLE_BROWSER_PROOF' in task['output'] and 'END_BROWSER_PROOF' in task['output'],task
            files=list((state/'.tool-results').glob('*.txt'))
            assert len(files)==1 and files[0].read_text().rstrip('\n')==PAGE, files
            assert counts['pages']==[0,4000,8000,12000,16000],counts
            print('PASS: one browser MCP evaluation -> complete 18000-byte result -> five contiguous pages -> middle/end proof -> DONE')
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
            provider.shutdown()
