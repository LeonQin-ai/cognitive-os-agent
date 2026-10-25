"""Opt-in configured API: Agent reads/writes with a real pending approval."""
import argparse,json,os,secrets,socket,subprocess,tempfile,time,urllib.request
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--config',required=True);args=p.parse_args()
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
config=json.loads(Path(args.config).read_text(encoding='utf-8-sig'));report={'model':config.get('llm.model')}
with tempfile.TemporaryDirectory(prefix='coa-access-live-') as folder:
    state=Path(folder);marker=secrets.token_hex(12);(state/'marker.txt').write_text(marker,encoding='utf8')
    config.update({'auth.key':'','http.bind':'127.0.0.1','reasoning.max_rounds':8})
    (state/'cognitive-os-agent.json').write_text(json.dumps(config),encoding='utf8')
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    def req(method,path,body=None):
        r=urllib.request.Request(f'http://127.0.0.1:{port}'+path,method=method,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(r,timeout=10) as response:return json.load(response)
    with (build/'desktop-access-live.log').open('w',encoding='utf8') as log:
        server=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',folder,'--workspace',folder,'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        tid=None;approved=set();started=time.monotonic()
        try:
            for _ in range(100):
                try:req('GET','/v1/scheduler');break
                except OSError:assert server.poll() is None;time.sleep(.05)
            tid=req('POST','/v1/chat',{'session':'approval-live','permission_mode':'auto','message':'仅使用 file_read 和 file_write：读取 marker.txt，将原文写入 proof.txt，再读取 proof.txt 核对。不要调用其他工具。完成后简短报告。'})['id']
            while True:
                task=req('GET',f'/v1/tasks/{tid}')
                report.update({k:task.get(k) for k in ('status','llm_calls','tool_calls','tokens_in','tokens_out','usage_available','model_failures')})
                if task['status'] in ('DONE','FAILED','CANCELLED','TIMEOUT'):break
                assert time.monotonic()-started<90,'live test exceeded 90 seconds'
                approval=task.get('approval')
                if approval and approval['id'] not in approved:
                    action=json.loads(approval['args']);target=(state/action['path']).resolve()
                    assert approval['tool']=='file_write' and target==state/'proof.txt',approval
                    assert not target.exists(),'write happened before approval'
                    req('POST',f'/v1/tasks/{tid}/approval',{'session':'approval-live','id':approval['id'],'allow':True});approved.add(approval['id'])
                time.sleep(.2)
            report.update(approvals=len(approved),elapsed_seconds=round(time.monotonic()-started,3))
            report['workspace']=task.get('workspace')
            report['actual_bytes_hex']=(state/'proof.txt').read_bytes().hex() if (state/'proof.txt').exists() else None
            report['expected_bytes_hex']=marker.encode().hex()
            (build/'desktop-access-live-task.json').write_text(json.dumps(task,ensure_ascii=False),encoding='utf8')
            report['artifact_verified']=(state/'proof.txt').exists() and (state/'proof.txt').read_text(encoding='utf8')==marker
            assert task['status']=='DONE' and approved and report['artifact_verified'],report
        finally:
            if tid is not None and report.get('status') not in ('DONE','FAILED','CANCELLED','TIMEOUT'):
                try:req('DELETE',f'/v1/tasks/{tid}')
                except OSError:pass
            (build/'desktop-access-live-report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
            server.terminate();server.wait(timeout=10)
print(json.dumps(report,ensure_ascii=False))
