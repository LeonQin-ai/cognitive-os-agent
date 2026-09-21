"""Offline end-to-end regression for task-bound progress and cancellation.

python tests/test_progress.py build/cognitive-os-agent[.exe]
--serve starts an isolated manual UI fixture on port 18431 (no real credentials).
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Model(BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        prompt = '\n'.join(str(m.get('content', '')) for m in request.get('messages', []))
        current = prompt.rsplit('## Current request\n', 1)[-1]
        observations = prompt.split('## 之前轮次的动作结果', 1)[-1].split('## 重要约束', 1)[0] if '## 之前轮次的动作结果' in prompt else ''
        time.sleep(0.4)
        if 'FAIL_FIXTURE' in current:
            self.send_response(500); self.end_headers(); return
        if 'STEER_FIXTURE' in current:
            content = '已调整任务：采用用户补充要求。'
        elif 'MOCK_STEP_DONE' in observations:
            content = '验证完成：工具已执行，执行步骤已保留。'
        elif 'SLOW_FIXTURE' in current:
            seconds = 8 if SERVE else 2
            content = json.dumps([{'tool': 'shell', 'args': {
                'command': f'python -c "import time; time.sleep({seconds}); print(\'MOCK_STEP_DONE\')"',
                'timeout_ms': 15000}}])
        else:
            content = '你好，任务已完成。'
        body = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': content},
                                      'finish_reason': 'stop'}],
                           'usage': {'prompt_tokens': 20, 'completion_tokens': 10}}).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body))); self.end_headers(); self.wfile.write(body)


def request(path, data=None, method=None):
    raw = json.dumps(data).encode() if data is not None else None
    req = urllib.request.Request(BASE+path, data=raw, method=method,
                                 headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=20) as response:
        return json.load(response)


def submit(session, message):
    return request('/v1/chat', {'session': session, 'message': message})['id']


def wait(task_id, session, cancel=False):
    stages = set(); seq = 0; deadline = time.monotonic()+45; sent_cancel = False
    while time.monotonic() < deadline:
        task = request(f'/v1/tasks/{task_id}')
        assert task.get('session_id') == session, task
        if task.get('seq'):
            assert task['task_id'] == task_id and task['run_id'] == task_id
            assert task['seq'] >= seq
            seq = task['seq']
        stages.add(task.get('stage'))
        assert 'process_log' not in task
        if cancel and task.get('cur_tool') and not sent_cancel:
            request(f'/v1/tasks/{task_id}', method='DELETE'); sent_cancel = True
        if task['status'] in ('DONE','FAILED','CANCELLED','TIMEOUT'):
            return task, stages
        time.sleep(0.08)
    raise AssertionError('task did not terminate')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(); parser.add_argument('binary'); parser.add_argument('--serve', action='store_true')
    args = parser.parse_args(); SERVE = args.serve
    binary = str(Path(args.binary).resolve())
    model = ThreadingHTTPServer(('127.0.0.1', 0), Model)
    threading.Thread(target=model.serve_forever, daemon=True).start()
    # Port 0 binding selects an available API port for automated tests.
    import socket
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0)); api_port = 18431 if SERVE else sock.getsockname()[1]
    BASE = f'http://127.0.0.1:{api_port}'
    with tempfile.TemporaryDirectory(prefix='cognitive-progress-') as workspace:
        with open(Path(workspace)/'server.log','w',encoding='utf-8') as log:
            env = dict(os.environ)
            env.pop('OPENAI_API_KEY',None); env.pop('ANTHROPIC_API_KEY',None)
            server = subprocess.Popen([binary,'serve',str(api_port)],cwd=workspace,env=env,stdout=log,stderr=log)
            try:
                for _ in range(100):
                    try: request('/v1/config/llm'); break
                    except Exception: time.sleep(0.1)
                request('/v1/config/llm', {'provider':'openai','base_url':f'http://127.0.0.1:{model.server_port}',
                                          'model':'progress-fixture','api_key':'local-test-only'})
                if SERVE:
                    print(f'UI fixture: {BASE} — send SLOW_FIXTURE',flush=True)
                    while True: time.sleep(1)
                first = submit('progress-a','SLOW_FIXTURE')
                second = submit('progress-b','hello')
                a, stages = wait(first,'progress-a')
                b, _ = wait(second,'progress-b')
                assert a['status'] == b['status'] == 'DONE', (a,b)
                assert 'executing' in stages, stages
                assert a['steps'] and any(step['ok'] == 1 for step in a['steps']), a
                assert b.get('steps') == [], b
                retained = request(f'/v1/tasks/{first}')
                assert retained['steps'] == a['steps'], retained
                saved = None
                for _ in range(25):
                    journal = request('/v1/tasks/journal?limit=20')
                    saved = next((row for row in journal if row['id'] == first), None)
                    if saved: break
                    time.sleep(0.04)
                assert saved, journal
                assert saved.get('trace') and any(e.get('stage') == 'executing' for e in saved['trace']), saved
                for event in saved['trace']:
                    assert all('args' not in step and 'out' not in step for step in event.get('steps', [])), event
                cancelled, _ = wait(submit('progress-c','SLOW_FIXTURE'),'progress-c',cancel=True)
                assert cancelled['status'] == 'CANCELLED', cancelled
                steering = submit('progress-s','SLOW_FIXTURE')
                for _ in range(100):
                    task = request(f'/v1/tasks/{steering}')
                    if task.get('cur_tool'): break
                    time.sleep(0.05)
                ack = request(f'/v1/tasks/{steering}/messages', {'session':'progress-s','message':'STEER_FIXTURE'})
                assert ack['revision'] == 1
                adjusted, _ = wait(steering,'progress-s')
                assert adjusted['status'] == 'DONE' and '已调整任务' in adjusted['output'], adjusted
                assert adjusted['applied_updates'] == 1 and adjusted['steps'], adjusted
                failed, _ = wait(submit('progress-f','FAIL_FIXTURE'),'progress-f')
                assert failed['status'] == 'FAILED' and failed.get('output'), failed
                print('PASS: isolation, live tools, terminal trace, sequence, retained steps, cancellation, in-flight steering, failure output')
            finally:
                server.terminate()
                try: server.wait(timeout=5)
                except subprocess.TimeoutExpired: server.kill(); server.wait()
                model.shutdown()
