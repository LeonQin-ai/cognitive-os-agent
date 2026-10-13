"""Real HTTP/scheduler/isolated-agent dependency gates, using a local LLM fixture."""
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')
entered=threading.Event()
release=threading.Event()
counts={}
errors=[]

class Provider(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text=body['messages'][-1]['content']
        current=text.split('## Current request\n',1)[-1]
        current=current.split('## 用户任务\n',1)[-1].split('\n## ',1)[0]
        try:
            if 'FLOW_DECOMP_MIXED' in current:
                answer=json.dumps([
                    {'agent':'alpha','task':'读取 evidence.c，完成 FLOW_WAIT_STEP1'},
                    {'agent':'alpha','task':'创建 api-step2.txt FLOW_CHILD 上游 {{step1}}'},
                    {'agent':'alpha','task':'创建 api-step3.txt FLOW_JOIN {{step2}}','after':[2]}])
            elif 'FLOW_SUMMARY' in current:
                assert '文件已生成' in current
                counts['summary']=counts.get('summary',0)+1
                answer='只总结上游文件交付结果，文字总结已完成。'
            elif 'FLOW_JOIN' in current:
                assert counts.get('child',0)>=2
                n=counts.get('join',0); counts['join']=n+1
                answer=json.dumps([{'tool':'file_write','args':{'path':'api-step3.txt','content':'after-second'}}]) if n==0 else '第三步文件已生成。'
            elif 'FLOW_WAIT_STEP1' in current:
                n=counts.get('upstream',0); counts['upstream']=n+1
                if n==0:
                    entered.set(); assert release.wait(15), 'fixture was not released'
                    answer=json.dumps([{'tool':'file_read','args':{'path':'evidence.c'}}])
                else:
                    assert 'READ_EVIDENCE' in text
                    answer='UPSTREAM_READY'
            elif 'FLOW_FAIL_STEP1' in current:
                counts['failed']=counts.get('failed',0)+1
                answer=json.dumps([{'tool':'file_read','args':{'path':'absent.c'}}])
            elif 'FLOW_CHILD' in current:
                n=counts.get('child',0); counts['child']=n+1
                assert 'UPSTREAM_READY' in current and release.is_set()
                answer=json.dumps([{'tool':'file_write','args':{'path':'api-step2.txt','content':'after-ready'}}]) if n==0 else '文件已生成。'
            elif 'FLOW_NEVER' in current:
                counts['never']=counts.get('never',0)+1
                answer=json.dumps([{'tool':'file_write','args':{'path':'DO_NOT_CREATE.txt','content':'bad'}}])
            elif 'FLOW_INDEPENDENT' in current:
                n=counts.get('independent',0); counts['independent']=n+1
                answer=json.dumps([{'tool':'file_write','args':{'path':'independent-api.txt','content':'independent-evidence'}}]) if n==0 else '独立文件已生成。'
            else:
                answer='任务未完成。'
        except Exception as error:
            errors.append(str(error)); answer='测试规划失败。'
        data=json.dumps({'choices':[{'message':{'role':'assistant','content':answer},'finish_reason':'stop'}]}).encode()
        self.send_response(200); self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)

backend=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coa-flow-deps-') as folder:
    state=Path(folder)
    (state/'evidence.c').write_bytes(b'READ_EVIDENCE')
    provider=ThreadingHTTPServer(('127.0.0.1',0),Provider)
    threading.Thread(target=provider.serve_forever,daemon=True).start()
    (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'flow-test',
        'llm.api_key':'local-test-only','llm.base_url':f'http://127.0.0.1:{provider.server_port}',
        'reasoning.max_rounds':4}),encoding='utf-8')
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1',0)); port=reservation.getsockname()[1]
    def request(path,body=None):
        req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,
            data=json.dumps(body).encode() if body is not None else None,
            headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(req,timeout=5) as response: return json.load(response)
    def finished(task_id):
        deadline=time.monotonic()+20
        while True:
            task=request('/v1/tasks/'+str(task_id))
            if task['status'] in ('DONE','FAILED','CANCELLED','TIMEOUT'): return task
            assert time.monotonic()<deadline,task
            time.sleep(.05)
    with (backend/'build/flow-dependencies-e2e.log').open('w',encoding='utf-8') as log:
        process=subprocess.Popen([str(backend/'build/cognitive-os-agent.exe'),'--state',str(state),
            '--workspace',str(state),'serve',str(port)],cwd=backend,stdout=log,stderr=log,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            deadline=time.monotonic()+20
            while True:
                try: request('/v1/scheduler'); break
                except OSError:
                    assert time.monotonic()<deadline and process.poll() is None
                    time.sleep(.1)
            request('/v1/agents',{'name':'alpha','role':'源码阅读与文件执行'})
            dag={'nodes':[{'id':'a','agent':'alpha','task':'读取 evidence.c，完成 FLOW_WAIT_STEP1'},
                {'id':'b','agent':'alpha','task':'创建 api-step2.txt，FLOW_CHILD，上游结果 {{a}}'}],
                'edges':[]}
            started=request('/v1/flows',dag)
            assert entered.wait(10)
            live=request('/v1/tasks/'+str(started['id']))
            nodes={n['id']:n for n in live['flow_nodes']}
            assert nodes['a']['status']=='running' and nodes['b']['status']=='queued',live
            assert counts.get('child',0)==0 and not (state/'api-step2.txt').exists()
            release.set()
            task=finished(started['id'])
            assert task['status']=='DONE',task
            assert (state/'api-step2.txt').read_text()=='after-ready'
            assert not errors,errors
            print('PASS: second step stays queued until first step completes with real source evidence')
            dag={'nodes':[{'id':'bad','agent':'alpha','task':'完整读取 absent.c 源码 FLOW_FAIL_STEP1'},
                {'id':'child','agent':'alpha','task':'创建 DO_NOT_CREATE.txt FLOW_NEVER'},
                {'id':'grandchild','agent':'alpha','task':'创建 DO_NOT_CREATE.txt FLOW_NEVER'},
                {'id':'independent','agent':'alpha','task':'创建 independent-api.txt FLOW_INDEPENDENT'}],
                'edges':[['bad','child'],['child','grandchild']]}
            task=finished(request('/v1/flows',dag)['id'])
            assert task['status']=='FAILED',task
            nodes={n['id']:n for n in task['flow_nodes']}
            assert nodes['bad']['status']=='error' and nodes['independent']['status']=='ok',task
            for name in ('child','grandchild'):
                assert nodes[name]['status']=='blocked' and nodes[name]['start_ms']==0,task
            assert counts.get('never',0)==0 and not (state/'DO_NOT_CREATE.txt').exists()
            assert (state/'independent-api.txt').exists()
            assert '节点未执行' in task['output'] and not errors
            print('PASS: failed upstream blocks descendants, independent branch completes, task is FAILED')
            # Mixed explicit/missing after values must still form a real chain.
            counts['upstream']=0; counts['child']=0; entered.clear(); release.clear()
            (state/'api-step2.txt').unlink()
            compiled=request('/v1/flows/decompose',{'task':'FLOW_DECOMP_MIXED'})
            compiled=compiled.get('dag',compiled.get('flow',compiled))
            if isinstance(compiled,str): compiled=json.loads(compiled)
            assert {'from':'step1','to':'step2'} in compiled['edges'],compiled
            auto_started=request('/v1/flows',compiled)
            assert entered.wait(10)
            live=request('/v1/tasks/'+str(auto_started['id']))
            nodes={n['id']:n for n in live['flow_nodes']}
            assert nodes['step1']['status']=='running' and nodes['step2']['status']=='queued' and nodes['step3']['status']=='queued',live
            assert not (state/'api-step2.txt').exists() and counts.get('child',0)==0
            release.set(); done=finished(auto_started['id'])
            assert done['status']=='DONE' and not errors,done
            nodes={n['id']:n for n in done['flow_nodes']}
            assert nodes['step2']['start_ms']>=nodes['step1']['end_ms']
            assert nodes['step3']['start_ms']>=nodes['step2']['end_ms']
            # A reused live registry slot must not erase the old terminal state.
            old=request('/v1/tasks/'+str(started['id']))
            assert {n['id'] for n in old['flow_nodes']}=={'a','b'},old
            print('PASS: auto-decomposed mixed dependencies execute strictly in order; terminal states persist')
            # Upstream verbs are context, not an extra write requirement for a summary.
            counts['child']=0
            dag={'nodes':[{'id':'writer','agent':'alpha','task':'创建 api-step2.txt FLOW_CHILD 上游 UPSTREAM_READY'},
                {'id':'summary','agent':'alpha','task':'总结上游结果 FLOW_SUMMARY {{writer}}'}]}
            summary=finished(request('/v1/flows',dag)['id'])
            assert summary['status']=='DONE' and '文字总结已完成' in summary['output'],summary
            assert counts.get('summary',0)==1 and not errors,errors
            print('PASS: summary completes without writing when upstream context says a file was generated')
        finally:
            release.set(); process.terminate(); process.wait(timeout=10)
            provider.shutdown(); provider.server_close()
