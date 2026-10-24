"""Opt-in configured API regression: isolated three-role, five-round IM room."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.request

parser = argparse.ArgumentParser()
parser.add_argument('--config', required=True)
args = parser.parse_args()
backend = Path(__file__).resolve().parents[1]
build = backend / os.environ.get('COA_BUILD_DIR', 'build')
binary = build / ('cognitive-os-agent.exe' if os.name == 'nt' else 'cognitive-os-agent')
config = json.loads(Path(args.config).read_text(encoding='utf-8-sig'))
report = {'model': config.get('llm.model'), 'status': 'not_started'}
with tempfile.TemporaryDirectory(prefix='coa-live-im-') as folder:
    state = Path(folder)
    config.update({'auth.key': '', 'http.bind': '127.0.0.1', 'reasoning.max_rounds': 4})
    (state / 'cognitive-os-agent.json').write_text(json.dumps(config), encoding='utf-8')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]

    def request(method, path, body=None):
        req = urllib.request.Request(f'http://127.0.0.1:{port}' + path, method=method,
            data=json.dumps(body).encode() if body is not None else None,
            headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, timeout=10) as response:
            return json.load(response)

    with (build / 'im-live-server.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen([str(binary), '--state', folder, '--workspace', folder, 'serve', str(port)],
            stdout=log, stderr=log, env={**os.environ, 'COA_TOOL_PROTOCOL': 'native'},
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        started = time.monotonic()
        try:
            for _ in range(100):
                try:
                    request('GET', '/v1/scheduler')
                    break
                except OSError:
                    assert process.poll() is None
                    time.sleep(.05)
            members = ['planner', 'reviewer', 'tester']
            for member in members:
                request('POST', '/v1/agents', {'name': member, 'role': member})
            room = request('POST', '/v1/im/sessions', {'name': 'live regression', 'kind': 'group',
                'members': members, 'discussion_rounds': 5})['id']
            task_id = request('POST', f'/v1/im/sessions/{room}/messages', {
                'sender': 'host', 'content': '讨论如何验证聊天消息加载可靠。每轮每人只答一句，回应上一人的观点。不使用工具，不修改文件；第五轮再结束讨论。'})['task_id']
            deadline = time.monotonic() + 180
            while True:
                task = request('GET', f'/v1/tasks/{task_id}')
                if task['status'] in ('DONE', 'FAILED', 'CANCELLED', 'TIMEOUT'):
                    break
                if time.monotonic() > deadline:
                    request('DELETE', f'/v1/tasks/{task_id}')
                    raise AssertionError('Discussion exceeded 180 seconds; cancelled to bound API cost')
                time.sleep(.3)
            messages = request('GET', f'/v1/im/sessions/{room}/messages')
            report['runtime_usage'] = request('GET', '/v1/usage')
            report.update({k: task.get(k) for k in ('status', 'discussion_round', 'discussion_rounds',
                'llm_calls', 'tool_calls', 'tokens_in', 'tokens_out', 'usage_available', 'model_failures')})
            report.update(elapsed_seconds=round(time.monotonic()-started, 3),
                replies=sum(m.get('role') == 'assistant' for m in messages))
            assert task['status'] == 'DONE' and report['discussion_round'] == 5 and report['replies'] == 15, report
            assert report['llm_calls'] >= report['replies'], report
            usage = report['runtime_usage']
            assert report['tokens_in'] == usage['total']['prompt'], report
            assert report['tokens_out'] == usage['total']['completion'] + usage['total'].get('reasoning', 0), report
            assert report['llm_calls'] == sum(m['calls'] for m in usage['models'].values()), report
        finally:
            (build / 'im-live-report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
            process.terminate()
            process.wait(timeout=10)
print(json.dumps(report, ensure_ascii=False))
