"""End-to-end metadata migration, provenance and restart regression (no model API)."""
import json,os,socket,subprocess,tempfile,time,urllib.request
from pathlib import Path
backend=Path(__file__).resolve().parents[1]
build=backend/os.environ.get('COA_BUILD_DIR','build')
build.mkdir(parents=True,exist_ok=True)
ark='https://ark.cn-beijing.volces.com/api/plan/v3'
with tempfile.TemporaryDirectory(prefix='coa-capabilities-') as tmp:
 state=Path(tmp)
 (state/'routes.json').write_text(json.dumps([{'name':'legacy','provider':'openai','model':'deepseek-v4.1-flash','base_url':ark,'context_window':262144}]),encoding='utf-8')
 (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'mock'}),encoding='utf-8')
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 def api(path,body=None):
  req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
  with urllib.request.urlopen(req,timeout=5) as response:return json.load(response)
 def route(name):return next(x for x in api('/v1/routes') if x['name']==name)
 def start(log):
  p=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',str(state),'--workspace',str(state),'serve',str(port)],cwd=backend,stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  for _ in range(100):
   try:api('/v1/scheduler');return p
   except OSError:assert p.poll() is None;time.sleep(.1)
  p.terminate();raise AssertionError('Startup timeout')
 def stop(p):
  p.terminate()
  try:p.wait(timeout=5)
  except subprocess.TimeoutExpired:p.kill();p.wait()
 with (build/'model-capabilities-e2e.log').open('w',encoding='utf-8') as log:
  p=start(log)
  try:
   x=route('legacy');assert x['context_window']==1048576 and x['context_source']=='catalog' and x['max_output_tokens']==393216,x
   body={'name':'legacy','provider':'openai','base_url':ark,'model':'deepseek-v4.1-flash','api_key':'fixture-key'}
   api('/v1/routes',{**body,'context_window':1500000,'max_output_tokens':4096,'context_source':'provider_metadata'})
   api('/v1/routes',{**body,'api_key':'new-fixture-key'})
   x=route('legacy');assert x['context_window']==1500000 and x['max_output_tokens']==4096 and x['context_source']=='provider_metadata',x
   api('/v1/routes',{**body,'context_window':262144,'context_source':'fallback'})
   assert route('legacy')['context_window']==1500000
   api('/v1/routes',{**body,'context_window':1048576,'context_source':'catalog'})
   assert route('legacy')['context_window']==1500000
   api('/v1/routes',{**body,'context_window':1500000,'context_source':'provider_metadata'})
   assert route('legacy')['max_output_tokens']==4096
  finally:stop(p)
  p=start(log)
  try:
   x=route('legacy');assert x['context_window']==1500000 and x['max_output_tokens']==4096 and x['context_source']=='provider_metadata',x
   api('/v1/routes',{'name':'unknown','provider':'openai','base_url':ark,'model':'unlisted-model'})
   x=route('unknown');assert x['context_window']==262144 and x['context_source']=='fallback',x
  finally:stop(p)
print('PASS: old fallback migration, metadata priority, key-only update, missing metadata retention, persistence and unknown-model provenance')
