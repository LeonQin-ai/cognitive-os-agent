"""Opt-in actual configured API: two conversation-local route aliases and settings."""
import argparse,json,os,socket,subprocess,tempfile,time,urllib.request
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--config',required=True);a=p.parse_args();root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build');config=json.loads(Path(a.config).read_text(encoding='utf-8-sig'));report={'model':config.get('llm.model'),'tasks':[]}
with tempfile.TemporaryDirectory(prefix='coa-live-isolation-') as folder:
 state=Path(folder,'private');state.mkdir();work=Path(folder,'work');work.mkdir();config.update({'auth.key':'','http.bind':'127.0.0.1','reasoning.max_rounds':5});(state/'cognitive-os-agent.json').write_text(json.dumps(config),encoding='utf8')
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 def req(method,path,body=None):
  r=urllib.request.Request(f'http://127.0.0.1:{port}'+path,method=method,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
  with urllib.request.urlopen(r,timeout=10) as response:return json.load(response)
 with (build/'session-isolation-live.log').open('w',encoding='utf8') as log:
  proc=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',str(state),'--workspace',str(work),'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0));tasks=[]
  try:
   for _ in range(100):
    try:req('GET','/v1/scheduler');break
    except OSError:assert proc.poll() is None;time.sleep(.05)
   for name,permission,thinking in (('window-a','ask',True),('window-b','full',False)):
    req('POST','/v1/routes',{'name':name,'provider':config['llm.provider'],'model':config['llm.model'],'base_url':config.get('llm.base_url',''),'api_key':config.get('llm.api_key','')})
    tid=req('POST','/v1/chat',{'message':'请只用一句中文说明你是AI助手，不调用工具。','session':name,'route':name,'permission_mode':permission,'thinking':thinking})['id'];tasks.append((tid,permission,thinking))
   end=time.monotonic()+90
   for tid,permission,thinking in tasks:
    while True:
     t=req('GET','/v1/tasks/'+str(tid))
     if t['status'] not in ('RUNNING','QUEUED'):break
     assert time.monotonic()<end,'live isolation exceeded time budget';time.sleep(.15)
    row={k:t.get(k) for k in ('id','status','model','permission_mode','thinking','llm_calls','tool_calls','tokens_in','tokens_out','model_failures')};report['tasks'].append(row)
    assert t['status']=='DONE' and t['permission_mode']==(1 if permission=='ask' else 0) and t['thinking']==thinking and t['tool_calls']==0,row
   report['global_model_unchanged']=req('GET','/v1/config/llm')['model']==config['llm.model'];report['usage']=req('GET','/v1/usage');assert report['global_model_unchanged']
  finally:
   for tid,_,_ in tasks:
    try:req('DELETE','/v1/tasks/'+str(tid))
    except OSError:pass
   (build/'session-isolation-live-report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8');proc.terminate();proc.wait(timeout=10)
print(json.dumps(report,ensure_ascii=False))
