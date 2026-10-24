"""Opt-in native-model observation checks through an isolated Runtime."""
import argparse, json, os, socket, subprocess, tempfile, time, urllib.request
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('--config',required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
config=json.loads(Path(args.config).read_text(encoding='utf-8-sig'))
results=[]
with tempfile.TemporaryDirectory(prefix='coa-observation-') as folder:
    config.update({'auth.key':'','http.bind':'127.0.0.1','reasoning.max_rounds':8})
    Path(folder,'cognitive-os-agent.json').write_text(json.dumps(config),encoding='utf-8')
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    def request(method,path,body=None):
        req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,method=method,
            data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(req,timeout=10) as r:return json.load(r)
    with (build/'observation-live.log').open('w',encoding='utf-8') as log:
        server=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),
            '--state',folder,'--workspace',folder,'serve',str(port)],stdout=log,stderr=log,
            env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            for _ in range(100):
                try:request('GET','/v1/scheduler');break
                except OSError:assert server.poll() is None;time.sleep(.05)
            cases=[('single','现在的时间',1),('sampling','请分三次独立调用工具读取系统时间，保存每一次原始结果，最后列出三个样本。不要合并为一次工具调用。',3)]
            for name,prompt,expected in cases:
                started=time.monotonic();tid=request('POST','/v1/chat',{'message':prompt,'session':name})['id']
                while True:
                    task=request('GET',f'/v1/tasks/{tid}')
                    if task['status'] in ('DONE','FAILED','CANCELLED','TIMEOUT'):break
                    if time.monotonic()-started>90:
                        request('DELETE',f'/v1/tasks/{tid}');raise AssertionError('Observation test cancelled after 90 seconds')
                    time.sleep(.2)
                result={k:task.get(k) for k in ('status','llm_calls','tool_calls','tokens_in','tokens_out','usage_available','model_failures')}
                result.update(case=name,elapsed_seconds=round(time.monotonic()-started,3))
                samples=[s for s in task.get('steps',[]) if s.get('tool')=='shell']
                result['sample_calls']=len(samples)
                results.append(result)
                (build/f'observation-{name}-task.json').write_text(json.dumps(task,ensure_ascii=False),encoding='utf-8')
                # Saving and verifying the requested samples may legitimately
                # use file tools; count independent probes, not all actions.
                assert task['status']=='DONE' and len(samples)==expected and all(s.get('ok') for s in samples) and task['model_failures']==0,result
        finally:
            (build/'observation-live-report.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf-8')
            server.terminate();server.wait(timeout=10)
print(json.dumps(results,ensure_ascii=False))
