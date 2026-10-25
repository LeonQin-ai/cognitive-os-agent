"""Opt-in real configured model smoke through an isolated Runtime and compiled CLI."""
import argparse, json, os, secrets, socket, subprocess, tempfile, time, urllib.request
from pathlib import Path

parser=argparse.ArgumentParser();parser.add_argument('--config',required=True);args=parser.parse_args()
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
binary=build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
configuration=json.loads(Path(args.config).read_text(encoding='utf-8-sig'))
report={'model':configuration.get('llm.model'),'status':'not_started'}
with tempfile.TemporaryDirectory(prefix='coa-cli-live-') as tmp:
    workspace=Path(tmp);state=workspace/'state';state.mkdir();proof=secrets.token_hex(12)
    (workspace/'marker.txt').write_text(proof,encoding='utf-8')
    configuration['auth.key']='';configuration['http.bind']='127.0.0.1';configuration['reasoning.max_rounds']=10
    (state/'cognitive-os-agent.json').write_text(json.dumps(configuration),encoding='utf-8')
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    base=f'http://127.0.0.1:{port}'
    def get(path):
        with urllib.request.urlopen(base+path,timeout=5) as r:return json.load(r)
    with (build/'cli-live-server.log').open('w',encoding='utf-8') as log:
        server=subprocess.Popen([str(binary),'--state',str(state),'--workspace',str(workspace),'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            for _ in range(100):
                try:get('/v1/scheduler');break
                except OSError:assert server.poll() is None;time.sleep(.05)
            prompt='读取当前工作区 marker.txt，将其内容原样写入 cli-proof.txt，再读取 cli-proof.txt 验证。完成后用一句中文报告文件内容。不要做其他操作。'
            start=time.monotonic()
            client=subprocess.run([str(binary),'chat','--connect',base,'-p',prompt],capture_output=True,text=True,encoding='utf-8',timeout=100,env={k:v for k,v in os.environ.items() if k!='COA_CLIENT_TOKEN'})
            # This isolated fresh Runtime starts its first task at ID 0.
            task=get('/v1/tasks/0')
            report.update({k:task.get(k) for k in ('status','llm_calls','tool_calls','tokens_in','tokens_out','usage_available','model_failures')})
            report.update(elapsed_seconds=round(time.monotonic()-start,3),client_exit=client.returncode,preview_observed='Provisional answer' in client.stderr)
            report['artifact_verified']=(workspace/'cli-proof.txt').exists() and (workspace/'cli-proof.txt').read_text(encoding='utf-8')==proof
            (build/'cli-live-output.txt').write_text(client.stdout,encoding='utf-8')
            (build/'cli-live-trace.txt').write_text(client.stderr,encoding='utf-8')
            assert client.returncode==0 and task['status']=='DONE' and report['artifact_verified'] and proof in client.stdout,report
        finally:
            (build/'cli-live-report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
            server.terminate();server.wait(timeout=10)
print(json.dumps(report,ensure_ascii=False))
