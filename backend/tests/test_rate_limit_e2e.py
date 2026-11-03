"""A provider 429 is reported as a rate limit and does not burn retry quota."""
import json,os,socket,subprocess,tempfile,threading,time,urllib.request
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
root=Path(__file__).resolve().parents[1];calls=0
class Provider(BaseHTTPRequestHandler):
 def log_message(self,*_):pass
 def do_POST(self):
  global calls
  calls+=1;self.rfile.read(int(self.headers.get('Content-Length','0')))
  body=b'{"error_msg":"429:v4_chat_completions_per_min_flow_control","error_code":"InferHub.00000429"}'
  self.send_response(429);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(body)));self.end_headers();self.wfile.write(body)
provider=ThreadingHTTPServer(('127.0.0.1',0),Provider)
threading.Thread(target=provider.serve_forever,daemon=True).start()
with tempfile.TemporaryDirectory(prefix='coa-rate-limit-') as folder:
 state=Path(folder);state.joinpath('cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixture','llm.api_key':'fixture-only','llm.base_url':f'http://127.0.0.1:{provider.server_port}','reasoning.max_rounds':8}),encoding='utf8')
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 binary=root/os.environ.get('COA_BUILD_DIR','build')/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
 with (root/'build-rate-limit.log').open('w') as log:
  proc=subprocess.Popen([str(binary),'--state',folder,'--workspace',folder,'serve',str(port)],cwd=root,stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  try:
   def api(path,body=None):
    req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=4) as response:return json.load(response)
   for _ in range(100):
    try:api('/v1/scheduler');break
    except OSError:assert proc.poll() is None;time.sleep(.1)
   task_id=api('/v1/chat',{'message':'Read the source and summarize','session':'rate-limit'})['id']
   for _ in range(200):
    task=api(f'/v1/tasks/{task_id}')
    if task['status'] not in ('QUEUED','RUNNING'):break
    time.sleep(.05)
   assert task['status']=='FAILED',task
   assert calls==1,calls
   assert '供应商返回 HTTP 429' in task['output'] and '模型规划或执行验证连续失败' not in task['output'],task['output']
   assert task['tool_calls']==0,task
   print('PASS: provider 429 gives specific resumable diagnosis and one request, no tool action')
  finally:proc.terminate();proc.wait(timeout=5)
provider.shutdown();provider.server_close()
