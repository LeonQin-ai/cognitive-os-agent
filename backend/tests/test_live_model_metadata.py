"""Actual provider metadata refresh through an isolated Runtime; no generation calls."""
import argparse,json,os,socket,subprocess,tempfile,time,urllib.request
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--routes',type=Path,required=True);p.add_argument('--report',type=Path,required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[1]
routes=json.loads(args.routes.read_text(encoding='utf-8-sig'))
selected=[r for r in routes if 'openrouter.ai/' in r.get('base_url','')]
assert selected,'No configured OpenRouter route'
with tempfile.TemporaryDirectory(prefix='coa-live-metadata-') as tmp:
 state=Path(tmp);(state/'routes.json').write_text(json.dumps(selected),encoding='utf8')
 (state/'cognitive-os-agent.json').write_text('{"llm.provider":"mock"}',encoding='utf8')
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 def api(path,body=None):
  req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
  with urllib.request.urlopen(req,timeout=10) as response:return json.load(response)
 args.report.parent.mkdir(parents=True,exist_ok=True)
 with args.report.with_suffix('.log').open('w') as log:
  proc=subprocess.Popen([str(root/'build'/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',tmp,'--workspace',tmp,'serve',str(port)],cwd=root,stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
  try:
   for _ in range(150):
    try:api('/v1/scheduler');break
    except OSError:assert proc.poll() is None;time.sleep(.1)
   api('/v1/routes/models',{'route_name':selected[0]['name']})
   deadline=time.monotonic()+50
   while True:
    result=api('/v1/routes/models')
    if not result.get('pending'):break
    assert time.monotonic()<deadline,'metadata timeout; no automatic retry'
    time.sleep(.3)
   assert result.get('ok'),result.get('error')
   updated=api('/v1/routes');safe=[{k:r.get(k) for k in ('name','model','context_window','context_source','max_output_tokens')} for r in updated]
   assert any(r['context_source']=='provider_metadata' for r in safe),safe
   report={'passed':True,'routes':safe,'listed_models':len(result['models']),'generation_requests':0,'tokens':0}
   args.report.write_text(json.dumps(report,indent=2),encoding='utf8');print(json.dumps(report))
  finally:proc.terminate();proc.wait(timeout=5)
