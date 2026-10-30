"""#202 follow-up: delivered analysis, not a promise after successful reads.

Both protocols exercise real HTTP, tools, task status and saved chat history.
The provider's next actions are scenario data, never runtime phrase rules.
"""
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
BINARY = BACKEND / os.environ.get('COA_BUILD_DIR', 'build') / (
    'cognitive-os-agent.exe' if os.name == 'nt' else 'cognitive-os-agent')
NARRATION = '根据已有信息，我需要继续读取关键文件的剩余部分......'
ANSWER = ('结果：process_state.h 只声明状态接口；process_state.cpp 的实现表明，'
          '进程重启通过 JOURNAL_REPLAY 恢复。单板重启需要 DISK_FLUSH 持久化，'
          '两者证据已分别从声明和实现中读取核实。')


class Provider(BaseHTTPRequestHandler):
    protocol = ''
    mode = ''
    calls = 0
    reviews = 0

    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        n = Provider.calls
        Provider.calls += 1
        review = 'COMPLETION REVIEW PROTOCOL:' in body['messages'][0]['content']
        Provider.reviews += review
        text, calls = '', []
        finish = lambda answer, status='complete': ('complete_task', {
            'status': status, 'requires_tools': False, 'answer': answer})
        if self.mode == 'chat':
            text = '你好！可以交流技术问题。'
            calls = [finish(text)] if self.protocol == 'native' else []
        elif self.mode.startswith('dir_'):
            target = 'nested/src/context' if self.mode != 'dir_unrelated' else 'other/context'
            if n == 0:
                calls = [('list_dir', {'path': 'src/context'})]
            elif n == 1:
                calls = [('file_read', {'path': target + '/proof.py'})] if self.mode == 'dir_readonly' else [('list_dir', {'path': target})]
            elif n == 2:
                calls = [('file_read', {'path': target + '/proof.py'})]
            else:
                text = 'Context 分析：CONTEXT_SOURCE_PROOF 已从真实源码读取并核实。'
                calls = [finish(text)] if self.protocol == 'native' or review else []
        elif n == 0:
            calls = [('file_read', {'path': 'process_state.h'})]
        elif n == 1:
            text = NARRATION
            calls = [finish(text)] if self.protocol == 'native' else []
        elif self.mode == 'stuck' or (self.mode == 'commentary' and n == 2):
            text = f'观察已收到；尚有工作等待处理，轮次{n}。'
        elif self.mode == 'blocked':
            calls = [finish('缺少目标系统的持久化配置，无法验证单板重启。', 'blocked')]
        elif n == (3 if self.mode == 'commentary' else 2):
            # The review resumes actual work. It cannot merely rename the
            # earlier progress announcement to "complete" in this fixture.
            calls = [('file_read', {'path': 'process_state.cpp'})]
        else:
            assert 'JOURNAL_REPLAY' in json.dumps(body) and 'DISK_FLUSH' in json.dumps(body)
            calls = [finish(ANSWER)]
        if self.protocol == 'native':
            message = {'role': 'assistant', 'content': text}
            if calls:
                message['tool_calls'] = [{'id': f'call-{n}-{i}', 'type': 'function',
                    'function': {'name': name, 'arguments': json.dumps(args)}}
                    for i, (name, args) in enumerate(calls)]
        else:
            message = {'role': 'assistant', 'content': json.dumps([
                {'tool': name, 'args': args} for name, args in calls]) if calls else text}
        data = json.dumps({'choices': [{'message': message,
            'finish_reason': 'tool_calls' if calls and self.protocol == 'native' else 'stop'}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def exercise(protocol, mode):
    Provider.protocol, Provider.mode, Provider.calls, Provider.reviews = protocol, mode, 0, 0
    fixture = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
    threading.Thread(target=fixture.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='coa-completion-review-') as folder:
            state = Path(folder)
            (state / 'process_state.h').write_text('void restore_state(void);', encoding='utf-8')
            (state / 'process_state.cpp').write_text('JOURNAL_REPLAY restores processes; DISK_FLUSH persists board state.', encoding='utf-8')
            for directory in ('nested/src/context', 'other/context'):
                (state / directory).mkdir(parents=True)
                (state / directory / 'proof.py').write_text('CONTEXT_SOURCE_PROOF', encoding='utf-8')
            if mode == 'dir_existing':
                (state / 'src').mkdir()
                (state / 'src/context').write_text('not a directory', encoding='utf-8')
            (state / 'cognitive-os-agent.json').write_text(json.dumps({
                'llm.provider': 'openai', 'llm.model': 'local-review-fixture',
                'llm.api_key': 'local-test-only',
                'llm.base_url': f'http://127.0.0.1:{fixture.server_port}',
                'reasoning.max_rounds': 2 if mode == 'budget' else 30}), encoding='utf-8')
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]

            def request(path, body=None):
                req = urllib.request.Request(f'http://127.0.0.1:{port}' + path,
                    data=json.dumps(body).encode() if body is not None else None,
                    headers={'Content-Type': 'application/json'})
                with urllib.request.urlopen(req, timeout=3) as response:
                    return json.load(response)

            with (state / 'runtime.log').open('w', encoding='utf-8') as log:
                process = subprocess.Popen([str(BINARY), '--state', folder,
                    '--workspace', folder, 'serve', str(port)], cwd=folder,
                    env={**os.environ, 'COA_TOOL_PROTOCOL': protocol}, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 20
                    while True:
                        try:
                            request('/v1/scheduler')
                            break
                        except OSError:
                            assert process.poll() is None and time.monotonic() < deadline
                            time.sleep(.03)
                    message = '你好' if mode == 'chat' else '分析这份代码中进程重启和单板重启的持久化能力。'
                    submitted = request('/v1/chat', {'message': message, 'session': 'review-case'})
                    while True:
                        task = request(f'/v1/tasks/{submitted["id"]}')
                        if task['status'] in ['DONE', 'FAILED', 'CANCELLED', 'TIMEOUT']:
                            break
                        assert time.monotonic() < deadline, task
                        time.sleep(.03)
                    if mode.startswith('dir_'):
                        expected_status = 'DONE' if mode == 'dir_relocated' else 'FAILED'
                        assert task['status'] == expected_status, task
                        assert task['steps'][0]['tool'] == 'list_dir' and not task['steps'][0]['ok'], task
                        assert Provider.calls <= 8, task
                        assert 'CONTEXT_SOURCE_PROOF' in task['output'], task
                        assert any(s['tool'] == 'file_read' and s['ok'] for s in task['steps']), task
                        if expected_status == 'FAILED':
                            assert 'src/context' in task['output'].replace('\\', '/'), task
                        history = request('/v1/chat/history?session=review-case')
                        assert bool(history['turns']) == (expected_status == 'DONE'), history
                        print(f'PASS: {protocol}/{mode}; {expected_status}, source proof and history verified', flush=True)
                        return
                    expected = {'recover': (4, 2, 'DONE'), 'commentary': (5, 2, 'DONE'),
                        'stuck': (7, 1, 'FAILED'), 'blocked': (3, 1, 'FAILED'),
                        'budget': (2, 1, 'FAILED'), 'chat': (1, 0, 'DONE')}[mode]
                    if mode in ('recover','commentary'):
                        expected=(expected[0]+1,expected[1],expected[2])
                    if protocol=='native' and mode=='budget':
                        expected=(3,1,'FAILED') # final review cannot perform the missing read
                    assert (task['llm_calls'], task['tool_calls'], task['status']) == expected, task
                    assert Provider.calls == expected[0], Provider.calls
                    if expected[1]:
                        assert task['steps'][0]['ok'] and task['steps'][0]['tool'] == 'file_read', task
                    if mode in ['recover', 'commentary']:
                        assert task['output'] == ANSWER, task
                        assert all(marker in task['steps'][-1]['out'] for marker in ['JOURNAL_REPLAY', 'DISK_FLUSH']), task
                    if protocol == 'legacy' and mode not in ['chat', 'budget']:
                        assert 1 <= Provider.reviews <= Provider.calls - 2, Provider.reviews
                    history = request('/v1/chat/history?session=review-case')
                    if task['status'] == 'DONE':
                        assert history['turns'][-1]['a'] == task['output'], history
                        assert NARRATION not in task['output'], task
                    else:
                        assert not history['turns'], history
                    print(f'PASS: {protocol}/{mode}; evidence, task status and chat history verified', flush=True)
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
    finally:
        fixture.shutdown()
        fixture.server_close()


if __name__ == '__main__':
    for protocol in ['legacy', 'native']:
        for mode in ['recover', 'commentary', 'stuck', 'blocked', 'budget', 'chat', 'dir_relocated', 'dir_unrelated', 'dir_existing', 'dir_readonly']:
            exercise(protocol, mode)
