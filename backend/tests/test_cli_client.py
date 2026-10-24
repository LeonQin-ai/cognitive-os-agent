"""Exercise the compiled terminal client over real HTTP, without model/API costs."""
import json, os, signal, subprocess, threading, time
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

root = Path(__file__).resolve().parents[1]
binary = root / os.environ.get('COA_BUILD_DIR', 'build') / ('cognitive-os-agent.exe' if os.name == 'nt' else 'cognitive-os-agent')
answer = '中文完整答复\n' + 'long-answer ' * 10000 + 'END_OF_ANSWER'
posts, deletes, polls = [], [], []
fail, running, failed_task, usage = False, False, False, True
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def reply(self, obj, status=200):
        raw=json.dumps(obj).encode(); self.send_response(status);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
    def auth(self):
        assert self.headers.get('Authorization') == 'Bearer fixture-token'
    def do_POST(self):
        self.auth(); body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));posts.append((self.path,body))
        if self.path == '/v1/chat/sessions': self.reply({'id':'session-'+str(len(posts))})
        elif self.path == '/v1/workspaces': self.reply({'workspaces':[{'path':body['path']}]})
        elif self.path.endswith('/workspace'): self.reply({'ok':True})
        elif fail: self.reply({'error':'fixture failure'},401)
        else: self.reply({'id':42})
    def do_GET(self):
        self.auth()
        if self.path == '/v1/chat/sessions': return self.reply([{'id':'session-1'}])
        polls.append(self.path)
        active=running and not deletes
        self.reply({'status':'RUNNING' if active else ('FAILED' if failed_task else 'DONE'),'round':1,'llm_calls':1,'tool_calls':1,
            'steps':[{'tool':'shell','args':'COMMAND_ARGUMENTS','ok':1,'ms':12,'out':'COMMAND_PROOF\n\x1b[2J'}], 'output':answer if not active else '',
            'usage_available':usage,'tokens_in':123,'tokens_out':456})
    def do_DELETE(self): self.auth();deletes.append(self.path);self.reply({'ok':True})

server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
env={**os.environ,'COA_CLIENT_TOKEN':'fixture-token'}
args=[str(binary),'--workspace','D:/项目 A','chat','--connect',f'http://127.0.0.1:{server.server_port}']
def run(extra=(),text=None):
    return subprocess.run(args+list(extra),input=text,text=True,encoding='utf-8',capture_output=True,env=env,timeout=15)
try:
    p=run(['-p','Read source and explain']); assert p.returncode==0,p.stderr
    assert answer in p.stdout,'answer truncated';assert 'COMMAND_PROOF' in p.stderr
    assert p.stdout==answer+'\n','one-shot stdout must contain only the answer'
    assert 'COMMAND_ARGUMENTS' in p.stderr and 'ok | 12ms' in p.stderr
    assert '\x1b' not in p.stderr,'terminal escape leaked';assert 'input 123 / output 456' in p.stderr
    assert posts[0][1]['workspace']=='D:/项目 A';assert posts[1][1]['message']=='Read source and explain'
    before=len(posts)
    p=run(text='/workspace D:/other folder\n/new\n/sessions\n/session prior-session\nhello\n/resume 42\n/exit\n')
    assert p.returncode==0,p.stderr
    assert any(path=='/v1/workspaces' and b['path']=='D:/other folder' for path,b in posts[before:])
    assert any(path=='/v1/chat/sessions' and b.get('workspace')=='D:/other folder' for path,b in posts[before:]),'New Chat lost updated workspace'
    assert any(path=='/v1/chat' and b['session']=='prior-session' for path,b in posts[before:])
    assert any(path=='/v1/tasks/42/resume' for path,b in posts[before:])
    fail=True;p=run(['-p','fail']);assert p.returncode!=0 and '401' in p.stderr;fail=False
    failed_task=True;usage=False;p=run(['-p','partial result']);assert p.returncode==1 and answer in p.stdout and 'Tokens: unavailable' in p.stderr
    failed_task=False;usage=True
    p=run(['--session','../bad','-p','x']);assert p.returncode==2
    p=run(['-p','/unknown']);assert p.returncode==2
    p=run(['-p','']);assert p.returncode==2
    if os.name!='nt':
        running=True
        with subprocess.Popen(args+['-p','long task'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,env=env) as proc:
            deadline=time.monotonic()+5;baseline=len(polls)
            while len(polls)==baseline:
                assert time.monotonic()<deadline;time.sleep(.02)
            proc.send_signal(signal.SIGINT);out,err=proc.communicate(timeout=10)
            assert deletes==['/v1/tasks/42'];assert b'Cancellation requested' in err
    print('PASS: terminal HTTP client sessions, workspace, resume, auth, full Unicode answer, tools, tokens, error exits and safe rendering')
finally:server.shutdown();server.server_close()
