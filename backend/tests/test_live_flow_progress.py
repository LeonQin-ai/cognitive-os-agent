"""Opt-in configured API: automatic multi-agent progress, isolated from user tasks."""
import argparse,json,os,socket,subprocess,tempfile,time,urllib.request
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--config',required=True);p.add_argument('--workspace-test',action='store_true');args=p.parse_args()
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
config=json.loads(Path(args.config).read_text(encoding='utf-8-sig'));report={'model':config.get('llm.model')}
with tempfile.TemporaryDirectory(prefix='coa-flow-live-') as folder:
    config.update({'auth.key':'','http.bind':'127.0.0.1','reasoning.max_rounds':5})
    state=Path(folder,'state');state.mkdir();workspace=Path(folder,'workspace');workspace.mkdir()
    if args.workspace_test:(workspace/'marker.txt').write_bytes(b'FLOW_WORKSPACE_PROOF\r\n')
    Path(state,'cognitive-os-agent.json').write_text(json.dumps(config),encoding='utf8')
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    def req(method,path,body=None):
        r=urllib.request.Request(f'http://127.0.0.1:{port}'+path,method=method,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(r,timeout=10) as response:return json.load(response)
    with (build/'flow-progress-live.log').open('w',encoding='utf8') as log:
        server=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',str(state),'--workspace',folder,'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        tid=None;stages=set();observed=[];started=time.monotonic()
        try:
            for _ in range(100):
                try:req('GET','/v1/scheduler');break
                except OSError:assert server.poll() is None;time.sleep(.05)
            for name,role in [('architect','提出界面进度方案'),('reviewer','审查进度方案')]:req('POST','/v1/agents',{'name':name,'role':role})
            task_text='分成两个顺序子任务：architect 用一句话提出如何显示任务进度，reviewer 用一句话回应其建议，最后综合。不读写文件、不使用工具，每人最多一句话。'
            if args.workspace_test:task_text='严格分成两个顺序子任务。architect 先读取所选工作区 marker.txt，再用 file_write(copy_from=marker.txt,path=result.txt)原样复制；reviewer 读取 result.txt，确认其内容是 FLOW_WORKSPACE_PROOF。所有路径相对工作区，最后每人一句话汇总。不得修改其他文件。'
            req('POST','/v1/workspaces',{'path':str(workspace)})
            tid=req('POST','/v1/orchestrate',{'task':task_text,'workspace':str(workspace)})['id']
            while True:
                task=req('GET',f'/v1/tasks/{tid}');stages.add(task.get('stage','queued'))
                if task['status']=='RUNNING' and task.get('flow_nodes'):
                    observed.append([(n['id'],n['status'],n.get('stage')) for n in task['flow_nodes']])
                report.update(status=task['status'],stages=sorted(stages),live_node_samples=len(observed))
                if task['status'] in ('DONE','FAILED','CANCELLED','TIMEOUT'):break
                assert time.monotonic()-started<90,'automatic Flow exceeded 90 seconds'
                time.sleep(.15)
            report.update(usage=req('GET','/v1/usage'),elapsed_seconds=round(time.monotonic()-started,3),nodes=[{k:n.get(k) for k in ('id','agent','status','stage','llm_calls','tool_calls','tokens_in','tokens_out','model_failures')} for n in task.get('flow_nodes',[])])
            (build/'flow-progress-live-task.json').write_text(json.dumps(task,ensure_ascii=False),encoding='utf8')
            if args.workspace_test:
                report['exact_bytes']=(workspace/'result.txt').is_file() and (workspace/'result.txt').read_bytes()==(workspace/'marker.txt').read_bytes()
                records=req('GET','/v1/flows/list')['records'];report['persisted_record']=any(r['task_id']==tid and r['automatic'] and Path(r['workspace'])==workspace for r in records)
                assert task['status']=='DONE' and report['exact_bytes'] and report['persisted_record'] and not Path(folder,'result.txt').exists(),report
            assert task['status']=='DONE' and observed and len(report['nodes'])>=2 and all(n['status']=='ok' for n in report['nodes']),report
        finally:
            if tid is not None and report.get('status') not in ('DONE','FAILED','CANCELLED','TIMEOUT'):
                try:req('DELETE',f'/v1/tasks/{tid}')
                except OSError:pass
            (build/'flow-progress-live-report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
            server.terminate();server.wait(timeout=10)
print(json.dumps(report,ensure_ascii=False))
