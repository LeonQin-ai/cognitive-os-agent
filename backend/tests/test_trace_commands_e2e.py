"""Trace actual command, output, elapsed time and durable history; local API only."""
import json, os, socket, subprocess, tempfile, threading, time, urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
backend=Path(__file__).resolve().parents[1];build=backend/os.environ.get('COA_BUILD_DIR','build')
count=0
python='python' if os.name=='nt' else 'python3'
commands=[{'command':python+' -c "print(123456)"'}, {'command':python+' -c "import sys; print(7654321); sys.exit(7)"'}, {'command':python+' -c "import time; time.sleep(2)"','timeout_ms':100}, {'command':python+' -c "print(444444)"'}]
class Provider(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_POST(self):
  global count
  self.rfile.read(int(self.headers['Content-Length']));n=count;count+=1
  calls=[('shell',commands[n])] if n<4 else [('complete_task',{'status':'complete','requires_tools':False,'answer':'All four command observations were checked, including expected failure and timeout.'})]
  response={'choices':[{'message':{'content':'','tool_calls':[{'id':'c','type':'function','function':{'name':name,'arguments':json.dumps(args)}} for name,args in calls]},'finish_reason':'tool_calls'}]}
  raw=json.dumps(response).encode();self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
with tempfile.TemporaryDirectory(prefix='coa-trace-commands-',ignore_cleanup_errors=True) as folder:
 state=Path(folder);server=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=server.serve_forever,daemon=True).start()
 (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'trace-fixture','llm.base_url':f'http://127.0.0.1:{server.server_port}','llm.api_key':'fixture','reasoning.max_rounds':12}),encoding='utf-8')
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 def request(path,body=None):
  req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
  with urllib.request.urlopen(req,timeout=5) as response:return json.load(response)
 def start(log):
  p=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',folder,'--workspace',folder,'serve',str(port)],cwd=backend,stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  for _ in range(100):
   try:request('/v1/scheduler');return p
   except OSError:
    if p.poll() is not None:
     log.flush()
     raise AssertionError('Agent startup failed: '+(build/'trace-commands.log').read_text(encoding='utf-8',errors='replace')[-4000:])
    time.sleep(.1)
  raise AssertionError('startup failed')
 def check(spans):
  spans=[s for s in spans if s['name']=='tool.shell'];assert len(spans)==4
  assert [s['command'] for s in spans]==[c['command'] for c in commands]
  assert [s['status'] for s in spans]==[1,-1,-1,1]
  assert '123456' in spans[0]['result'] and 'exit code 7' in spans[1]['result'] and '7654321' in spans[1]['result']
  assert 'timeout' in spans[2]['result'] and spans[2]['duration_ms']>=80 and '444444' in spans[3]['result']
  assert all(s['end_ms']>=s['start_ms'] and s['duration_ms']>=0 for s in spans)
 with (build/'trace-commands.log').open('w',encoding='utf-8') as log:
  p=start(log)
  try:
   task=request('/v1/chat',{'message':'Execute the four fixture probes and report actual observations.','session':'trace-fixture'});deadline=time.monotonic()+40
   while True:
    task=request('/v1/tasks/'+str(task['id']))
    if task['status'] not in ('RUNNING','QUEUED','CANCELLING'):break
    assert time.monotonic()<deadline;time.sleep(.05)
   assert task['status']=='DONE',task.get('output')
   check(request('/v1/trace?task_id='+str(task['id'])))
   deadline=time.monotonic()+5
   while True:
    history=request('/v1/trace/history?task_id='+str(task['id']))
    if history:break
    assert time.monotonic()<deadline;time.sleep(.05)
   check(history[0]['spans'])
  finally:p.terminate();p.wait(timeout=5)
  # Persistence concerns state, not immediate POSIX listener rebinding.
  with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
  p=start(log)
  try:check(request('/v1/trace/history?task_id='+str(task['id']))[0]['spans'])
  finally:p.terminate();p.wait(timeout=5);server.shutdown();server.server_close()
 print('PASS: command/results/durations for success, exit 7, timeout and restart persistence',flush=True)
