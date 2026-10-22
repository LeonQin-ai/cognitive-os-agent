"""#202: single-response degeneration must never become a successful answer."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BACKEND = Path(__file__).resolve().parents[1]
BINARY = BACKEND / os.environ.get('COA_BUILD_DIR', 'build') / (
    'cognitive-os-agent.exe' if os.name == 'nt' else 'cognitive-os-agent')


class Provider(BaseHTTPRequestHandler):
    answer = ''
    native = False
    calls = 0
    tool_first = False

    def log_message(self, *args):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers['Content-Length']))
        Provider.calls += 1
        message = {'role': 'assistant', 'content': self.answer}
        if self.native:
            message = {'role': 'assistant', 'content': '', 'tool_calls': [{
                'id': 'completion', 'type': 'function', 'function': {
                    'name': 'complete_task', 'arguments': json.dumps({
                        'status': 'complete', 'requires_tools': False,
                        'answer': self.answer})}}]}
        if self.tool_first and Provider.calls == 1:
            call = {'name': 'file_read', 'arguments': json.dumps({'path': 'evidence.txt'})}
            message = ({'role': 'assistant', 'content': '', 'tool_calls': [{
                'id': 'read-evidence', 'type': 'function', 'function': call}]}
                if self.native else {'role': 'assistant', 'content': json.dumps([
                    {'tool': 'file_read', 'args': {'path': 'evidence.txt'}}])})
        data = json.dumps({'choices': [{'message': message,
                                       'finish_reason': 'stop'}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    cycle = ''.join(f'grep 搜索 "{word}" 相关关键词：' for word in
                    ['persist', 'restart', 'reboot', '单板', 'restore', 'snapshot'])
    cases = [
        ('issue-202', '我将搜索相关代码。' + cycle * 18, True),
        ('after-tools', '我将搜索相关代码。' + cycle * 18, True),
        ('english-cycle', 'Inspect source, then check recovery evidence. ' * 40, True),
        ('single-character', 'x' * 2000, True),
        ('short-quote', cycle * 2, False),
        ('distinct-analysis', '\n'.join(
            f'Section {i}: evidence at src/module_{i}/store.c:{i*17} '
            f'has value {i*i*i}; recovery result {i*123457}.' for i in range(80)), False),
        ('quoted-repetition', 'x' * 800 + '\n'.join(
            f'Unique evidence {i}: {i*i}, path src/{i}/state.c' for i in range(100)), False),
    ]
    server = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for protocol in ['legacy', 'native']:
            Provider.native = protocol == 'native'
            for name, answer, rejected in cases:
                Provider.answer, Provider.calls = answer, 0
                Provider.tool_first = name == 'after-tools'
                with tempfile.TemporaryDirectory(prefix='coa-repetition-') as folder:
                    state = Path(folder)
                    (state / 'evidence.txt').write_text('verified source evidence', encoding='utf-8')
                    config = state / 'cognitive-os-agent.json'
                    config.write_text(json.dumps({
                        'llm.provider': 'openai', 'llm.model': 'local-fixture',
                        'llm.base_url': f'http://127.0.0.1:{server.server_port}',
                        'reasoning.max_rounds': 4}), encoding='utf-8')
                    result = subprocess.run([str(BINARY), '--config', str(config),
                        '--workspace', folder, 'run', '你好'],
                        cwd=folder, env={**os.environ, 'COA_TOOL_PROTOCOL': protocol},
                        capture_output=True, encoding='utf-8', timeout=20)
                    assert (result.returncode != 0) == rejected, (name, result.stdout, result.stderr)
                    assert Provider.calls == (2 if Provider.tool_first else 1), (name, Provider.calls)
                    if rejected:
                        assert '持续重复' in result.stdout, result.stdout
                        assert answer not in result.stdout, name
                    else:
                        assert answer in result.stdout, (name, result.stdout)
                    print(f'PASS: {protocol}/{name}', flush=True)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
