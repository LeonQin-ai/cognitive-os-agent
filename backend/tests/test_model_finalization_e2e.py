"""#201: preserve recovery evidence and bound unusable token-limit responses.

Local providers exercise actual HTTP, tools, scheduler and task diagnostics.
No real model, credentials, task-word classifiers or relaxed timeouts.
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
SECRET_THINKING = 'PRIVATE_REASONING_MUST_NOT_BE_DELIVERED_OR_REPLAYED'


class Provider(BaseHTTPRequestHandler):
    provider = 'openai'
    mode = ''
    requests = []

    def log_message(self, *args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        n = len(Provider.requests)
        Provider.requests.append(request)
        assert SECRET_THINKING not in json.dumps(request), 'Private thinking leaked into context'
        if n == 3 and self.mode in ['http-recover', 'json-recover']:
            data = b'{"error":{"message":"local fixture unavailable"}}' if self.mode == 'http-recover' else b'{invalid JSON'
            self.send_response(503 if self.mode == 'http-recover' else 200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        calls = []
        text = ''
        truncated = False
        if n == 0:
            calls = [('shell', {'command': 'exit 7'})]
        elif n == 1:
            python = 'python' if os.name == 'nt' else 'python3'
            calls = [('shell', {'command': python + ' -c "from pathlib import Path; '
                               "Path('proof.txt').write_text('recovered-and-verified')\""})]
        elif n == 2:
            calls = [('file_read', {'path': 'proof.txt'})]
        elif self.mode in ['truncated', 'tool-limit'] or (self.mode == 'recover' and n == 3):
            # A syntactically complete declaration is still invalid when the
            # provider explicitly reports that generation was cut short.
            truncated = True
            calls = [('complete_task', {'status': 'complete', 'requires_tools': False,
                                       'answer': 'must not accept truncated completion'})]
            if self.mode == 'tool-limit':
                calls = [('file_write', {'path': 'must-not-execute.txt', 'content': 'invalid partial call'})]
        elif self.mode == 'thinking-limit':
            truncated = True
        elif self.mode == 'thinking-stop' and n == 3:
            pass
        else:
            calls = [('complete_task', {'status': 'complete', 'requires_tools': False,
                                       'answer': 'Recovered file was read and verified.'})]

        if self.provider == 'openai':
            message = {'role': 'assistant', 'content': text}
            if not calls:
                message['reasoning_content'] = SECRET_THINKING
            if calls:
                message['tool_calls'] = [{'id': f'call-{n}-{i}', 'type': 'function',
                    'function': {'name': name, 'arguments': json.dumps(args)}}
                    for i, (name, args) in enumerate(calls)]
            response = {'choices': [{'message': message,
                'finish_reason': 'length' if truncated else 'tool_calls' if calls else 'stop'}],
                'usage': {'prompt_tokens': 11, 'completion_tokens': 7}}
        else:
            content = [{'type': 'tool_use', 'id': f'call-{n}-{i}',
                        'name': name, 'input': args} for i, (name, args) in enumerate(calls)]
            if not calls:
                content = [{'type': 'thinking', 'thinking': SECRET_THINKING}]
            response = {'content': content,
                'stop_reason': 'max_tokens' if truncated else 'tool_use' if calls else 'end_turn',
                'usage': {'input_tokens': 11, 'output_tokens': 7}}
        data = json.dumps(response).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def exercise(provider, mode):
    Provider.provider, Provider.mode, Provider.requests = provider, mode, []
    fixture = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
    threading.Thread(target=fixture.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='coa-finalization-') as folder:
            state = Path(folder)
            (state / 'cognitive-os-agent.json').write_text(json.dumps({
                'llm.provider': provider, 'llm.model': 'local-finalization-fixture',
                'llm.base_url': f'http://127.0.0.1:{fixture.server_port}',
                'reasoning.max_rounds': 30}), encoding='utf-8')
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
                    env={**os.environ, 'COA_TOOL_PROTOCOL': 'native'}, stdout=log, stderr=log)
                try:
                    deadline = time.monotonic() + 15
                    while True:
                        try:
                            request('/v1/scheduler')
                            break
                        except OSError:
                            assert process.poll() is None and time.monotonic() < deadline
                            time.sleep(.03)
                    submitted = request('/v1/tasks', {'prompt': 'Perform the requested operation and report the evidence.'})
                    deadline = time.monotonic() + 20
                    while True:
                        task = request(f'/v1/tasks/{submitted["id"]}')
                        if task['status'] in ['DONE', 'FAILED', 'CANCELLED', 'TIMEOUT']:
                            break
                        assert time.monotonic() < deadline, task
                        time.sleep(.03)
                    failed = mode in ['truncated', 'tool-limit', 'thinking-limit']
                    assert task['status'] == ('FAILED' if failed else 'DONE'), task
                    assert task['tool_calls'] == 3, task
                    assert [step['ok'] for step in task['steps']] == [False, True, True], task
                    assert (state / 'proof.txt').read_text() == 'recovered-and-verified'
                    assert not (state / 'must-not-execute.txt').exists()
                    assert len(Provider.requests) == 6, (mode, len(Provider.requests), task)
                    assert task['llm_calls'] == 6, task
                    # Hidden-only exhaustion must not enlarge subsequent paid
                    # requests; genuine partial native calls retain escalation.
                    expected = [32768]*4 + ([65536]*2 if mode in ['recover','truncated','tool-limit'] else [32768]*2)
                    assert [r['max_tokens'] for r in Provider.requests] == expected, (mode, [r['max_tokens'] for r in Provider.requests])
                    reported = 5 if mode in ['http-recover', 'json-recover'] else 6
                    assert task['tokens_in'] == 11*reported and task['tokens_out'] == 7*reported, task
                    assert SECRET_THINKING not in task['output'], task
                    diagnostic = task['model_failure']
                    assert diagnostic['http_status'] == (503 if mode == 'http-recover' else 200), task
                    assert diagnostic['stage'] == ('http' if mode == 'http-recover' else 'response'), task
                    assert diagnostic['max_tokens'] > 0, task
                    assert diagnostic['has_tool_calls'] == (mode in ['truncated', 'tool-limit', 'recover']), task
                    assert not diagnostic['has_content'], task
                    was_truncated = mode in ['truncated', 'tool-limit', 'recover', 'thinking-limit']
                    if was_truncated:
                        assert Provider.requests[3]['max_tokens'] <= Provider.requests[4]['max_tokens'] <= 65536
                    assert diagnostic['truncated'] == was_truncated, task
                    expected_stop = ('length' if provider == 'openai' else 'max_tokens') if was_truncated else ('stop' if provider == 'openai' else 'end_turn') if mode == 'thinking-stop' else ''
                    assert diagnostic['stop_reason'] == expected_stop, task
                    assert diagnostic['call'] == (6 if failed else 4), task
                    if failed:
                        assert 'token' in task['output'] and '任务未完成' in task['output'], task
                    else:
                        assert task['output'] == 'Recovered file was read and verified.', task
                    print(f'PASS: {provider}/{mode}; recovered tools, bounded completion, safe diagnostics, exact usage', flush=True)
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
    for provider in ['openai', 'anthropic']:
        for mode in ['truncated', 'tool-limit', 'recover', 'thinking-limit', 'thinking-stop', 'http-recover', 'json-recover']:
            exercise(provider, mode)
