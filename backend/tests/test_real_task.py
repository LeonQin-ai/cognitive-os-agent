"""Opt-in real-provider acceptance test in an isolated temporary workspace.

Uses only llm.* values from the specified local config; never prints credentials.
The agent must fix actual Python code and run actual tests. Artifacts are retained.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument('binary'); parser.add_argument('--config', required=True)
parser.add_argument('--output', required=True)
args = parser.parse_args()
binary = str(Path(args.binary).resolve())
work = Path(args.output).resolve(); work.mkdir(parents=True,exist_ok=True)
(work/'RESULT.md').unlink(missing_ok=True)
(work/'verification.json').unlink(missing_ok=True)
state = work/'state'; state.mkdir(exist_ok=True)
source = json.loads(Path(args.config).read_text(encoding='utf-8'))
cfg = {k:v for k,v in source.items() if k.startswith('llm.')}
cfg.update({'reasoning.max_rounds':12,'scheduler.workers':2,'tx.use_transaction':False})
(state/'cognitive-os-agent.json').write_text(json.dumps(cfg),encoding='utf-8')
(work/'stats.py').write_text('def average(values):\n    return sum(values) / len(values)\n',encoding='utf-8')
(work/'test_stats.py').write_text('''from stats import average
assert average([2, 4, 6]) == 4
assert average([]) == 0
assert average([0]) == 0
print("REAL_TASK_CHECKS_PASSED")
''',encoding='utf-8')
with socket.socket() as sock:
    sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
base=f'http://127.0.0.1:{port}'
def request(path,data=None):
    req=urllib.request.Request(base+path,data=json.dumps(data).encode() if data is not None else None,
        headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=30) as r: return json.load(r)
env={k:v for k,v in os.environ.items() if not k.startswith('COA_')}
with (work/'server.log').open('w',encoding='utf-8') as log:
    server=subprocess.Popen([binary,'serve',str(port)],cwd=work,env=env,stdout=log,stderr=log)
    try:
        for _ in range(100):
            try: request('/v1/config/llm'); break
            except Exception: time.sleep(0.1)
        task_id=request('/v1/chat',{'session':'real-bugfix','message':
            '分析工作目录的 stats.py 和 test_stats.py，实际运行测试复现 bug，修复 average 对空列表的处理（返回 0），保留正常输入行为。再次运行测试验证。只能修改当前工作目录文件。最终简要说明原因、修改和真实验证结果。'})['id']
        print(f'Real-provider task started: {task_id}',flush=True)
        last=None; sent=False; reminded=False; deadline=time.monotonic()+300; snapshots=[]
        while time.monotonic()<deadline:
            t=request(f'/v1/tasks/{task_id}'); snapshots.append(t)
            stage=(t.get('stage'),t.get('cur_tool'),len(t.get('steps',[])))
            if stage!=last: print('Progress:',stage,flush=True); last=stage
            if not sent and t.get('stage') in ('planning','executing'):
                request(f'/v1/tasks/{task_id}/messages',{'session':'real-bugfix','message':
                    '补充要求：增加负数输入 average([-2, -4]) == -3 的测试；验证后把实际结论写入 RESULT.md，说明空列表、普通输入、负数三种情况。继续当前任务即可。'})
                sent=True; print('Steering message accepted',flush=True)
            if sent and not reminded and len(t.get('steps',[])) >= 10 and not (work/'RESULT.md').exists():
                request(f'/v1/tasks/{task_id}/messages',{'session':'real-bugfix','message':
                    '验收纠正：RESULT.md 仍不存在。下一步请先用 file_write 创建 RESULT.md，再做其他检查；不要只在最终回答中描述它。继续当前任务。'})
                reminded=True; print('Second steering correction accepted',flush=True)
            if t['status'] in ('DONE','FAILED','CANCELLED','TIMEOUT'): break
            time.sleep(0.5)
        (work/'progress.json').write_text(json.dumps(snapshots,ensure_ascii=False,indent=2),encoding='utf-8')
        assert t['status']=='DONE', (t['status'],t.get('output'))
        assert sent and reminded and t.get('applied_updates',0)>=2,'steering corrections not applied'
        checked=subprocess.run(['python','test_stats.py'],cwd=work,capture_output=True,text=True)
        independent=subprocess.run(['python','-c','from stats import average; assert average([])==0; assert average([2,4,6])==4; assert average([-2,-4])==-3'],cwd=work,capture_output=True,text=True)
        assert checked.returncode==independent.returncode==0,checked.stderr+independent.stderr
        assert (work/'RESULT.md').is_file(),'missing requested report'
        assert '-2' in (work/'test_stats.py').read_text(encoding='utf-8'),'negative case not added'
        report={'status':'passed','task_id':task_id,'steps':len(t.get('steps',[])),
                'applied_updates':t.get('applied_updates'),'independent_tests':'passed','answer':t.get('output')}
        (work/'verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        print('PASS: real model fixed real code, executed tests, followed steering, wrote RESULT.md',flush=True)
    finally:
        server.terminate()
        try: server.wait(timeout=5)
        except subprocess.TimeoutExpired: server.kill(); server.wait()
        # Do not retain copied credentials among test artifacts.
        (state/'cognitive-os-agent.json').unlink(missing_ok=True)
