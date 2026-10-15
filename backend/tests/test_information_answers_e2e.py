"""#139: information answers finish without tools; work still requires evidence."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


BACKEND = Path(__file__).resolve().parents[1]
BINARY = (BACKEND / os.environ.get('COA_BUILD_DIR', 'build') /
          ('cognitive-os-agent.exe' if os.name == 'nt' else 'cognitive-os-agent'))
TERMINAL = ('DONE', 'FAILED', 'CANCELLED', 'TIMEOUT')
SAFETY_ANSWER = (
    '我的操作有安全边界：权限取决于运行账户，执行记录可以核对。'
    '如果你让我执行危险命令，我需要先说明影响并确认目标范围。'
    '这些是安全机制的说明，当前没有请求执行任何命令。')
CASES = [
    ('safety', '你的操作安全吗', SAFETY_ANSWER, 'DONE'),
    ('permissions', '你的权限有哪些',
     '我需要说明权限边界：只能使用运行账户拥有的权限，工具操作受策略约束。', 'DONE'),
    ('english', 'Are your operations safe?',
     'Let me explain: operations use the current account permissions, and destructive '
     'changes require care. This is a description of the safety boundaries.', 'DONE'),
    ('create', '你的操作安全吗？请创建 safety.txt',
     '我需要说明安全边界；我将创建 safety.txt。', 'FAILED'),
    ('save', '你的权限有哪些？请把安全说明保存为 safety.md',
     '我需要说明权限边界；我将保存 safety.md。', 'FAILED'),
    ('ppt', '你的操作安全吗？请生成安全说明PPT',
     '我需要说明安全边界；我将生成安全说明PPT。', 'FAILED'),
    ('register', '你的操作安全吗？请注册 safety_check skill',
     '我需要说明安全边界；我将注册 safety_check skill。', 'FAILED'),
    ('inspect', '检查这个命令是否安全', '让我先检查这个命令。', 'FAILED'),
    ('connect', '你的操作安全吗？帮我连接 SSH 服务器',
     '我将连接 SSH 服务器。', 'FAILED'),
    ('download', '你的操作安全吗？请下载项目依赖',
     '我将下载项目依赖。', 'FAILED'),
    ('run', 'Are your operations safe? Run the command',
     'I will run the command.', 'FAILED'),
]


class Provider(BaseHTTPRequestHandler):
    calls = {}
    errors = []

    def log_message(self, *args):
        pass

    def do_POST(self):
        try:
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            text = body['messages'][-1]['content']
            # Shared retrieval can include earlier sessions. Match only the
            # current task, never a safety question in a retrieved episode.
            current = text.split('## Current request\n', 1)[-1].split('\n## ', 1)[0]
            case = next(case for case in sorted(CASES, key=lambda item: -len(item[1]))
                        if case[1] in current)
            self.calls[case[0]] = self.calls.get(case[0], 0) + 1
            answer = case[2]
            data = json.dumps({'choices': [{'message': {
                'role': 'assistant', 'content': answer}, 'finish_reason': 'stop'}]}).encode()
            self.send_response(200)
        except Exception as error:
            self.errors.append(repr(error))
            data = json.dumps({'error': str(error)}).encode()
            self.send_response(500)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    provider = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
    threading.Thread(target=provider.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='coa-information-answers-') as folder:
            state = Path(folder)
            (state / 'cognitive-os-agent.json').write_text(json.dumps({
                'llm.provider': 'openai', 'llm.model': 'information-answer-fixture',
                'llm.api_key': 'local-test-only', 'reasoning.max_rounds': 6,
                'llm.base_url': f'http://127.0.0.1:{provider.server_port}',
            }), encoding='utf-8')
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]

            def request(path, body=None):
                req = urllib.request.Request(f'http://127.0.0.1:{port}' + path,
                    data=json.dumps(body).encode() if body is not None else None,
                    headers={'Content-Type': 'application/json'})
                with urllib.request.urlopen(req, timeout=5) as response:
                    return json.load(response)

            def wait_for(read, ready, label):
                deadline = time.monotonic() + 20
                latest = None
                while time.monotonic() < deadline:
                    try:
                        latest = read()
                        if ready(latest):
                            return latest
                    except OSError:
                        assert process.poll() is None, f'Runtime exited during {label}'
                    time.sleep(.03)
                raise AssertionError(f'Timed out waiting for {label}: {latest!r}')

            def journal_record(task_id):
                return wait_for(lambda: request('/v1/tasks/journal'),
                    lambda records: any(record['id'] == task_id and
                                        record['status'] in TERMINAL for record in records),
                    'durable task record')

            with (state / 'runtime.log').open('w', encoding='utf-8') as log:
                def start():
                    return subprocess.Popen([str(BINARY), '--state', str(state),
                        '--workspace', str(state), 'serve', str(port)],
                        cwd=state, stdout=log, stderr=log,
                        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))

                def stop():
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)

                process = start()
                failures = []
                completed = []
                try:
                    wait_for(lambda: request('/v1/scheduler'), lambda _: True, 'startup')
                    for name, prompt, answer, expected in CASES:
                        session = f'information-139-{name}'
                        task_id = request('/v1/chat', {'message': prompt, 'session': session})['id']
                        task = wait_for(lambda: request('/v1/tasks/' + str(task_id)),
                            lambda value: value['status'] in TERMINAL, name)
                        records = journal_record(task_id)
                        record = next(record for record in records
                                      if record['id'] == task_id and record['status'] in TERMINAL)
                        history = request('/v1/chat/history?session=' + session)
                        completed.append((session, task, history))
                        try:
                            assert task['status'] == expected, task
                            assert task['stage'] == ('completed' if expected == 'DONE' else 'failed'), task
                            assert task['tool_calls'] == 0 and task.get('steps', []) == [], task
                            assert record['status'] == expected and record['output'] == task['output'], record
                            run = next(run for run in history['runs']
                                       if run['id'] == task_id and run['status'] in TERMINAL)
                            assert run['status'] == expected and run['output'] == task['output'], run
                            if expected == 'DONE':
                                assert task['output'] == answer, task
                                assert task['round'] == 1 and task['llm_calls'] == 1, task
                                assert Provider.calls.get(name) == 1, Provider.calls
                                assert history['turns'][-1]['q'] == prompt, history
                                assert history['turns'][-1]['a'] == answer, history
                            else:
                                assert '任务未完成' in task['output'], task
                                assert not (state / 'safety.txt').exists()
                                assert not (state / 'safety.md').exists()
                            print(f'PASS: {name} -> {expected}', flush=True)
                        except AssertionError as error:
                            failures.append(f'{name}: {error}')
                            print(f'FAIL: {name} -> {task["status"]} (expected {expected})', flush=True)

                    # Both terminal states and completed chat answers must
                    # survive a real Runtime restart with the same state root.
                    stop()
                    process = start()
                    wait_for(lambda: request('/v1/scheduler'), lambda _: True, 'restart')
                    for session, task, original_history in completed:
                        restored = request('/v1/chat/history?session=' + session)
                        assert restored['turns'] == original_history['turns'], restored
                        run = next(run for run in restored['runs']
                                   if run['id'] == task['id'] and run['status'] in TERMINAL)
                        assert run['status'] == task['status'] and run['output'] == task['output'], run
                        records = journal_record(task['id'])
                        record = next(record for record in records
                                      if record['id'] == task['id'] and record['status'] in TERMINAL)
                        assert record['status'] == task['status'] and record['output'] == task['output'], record
                    assert not Provider.errors, Provider.errors
                    assert not failures, '\n'.join(failures)
                    print('PASS: 3 information answers, 8 unexecuted work requests; '
                          'chat history and task status survive Runtime restart')
                finally:
                    stop()
    finally:
        provider.shutdown()
        provider.server_close()


if __name__ == '__main__':
    main()
