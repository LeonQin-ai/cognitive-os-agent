"""Real HTTP -> DSML planner -> tools -> persisted file, using a local provider."""
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BACKEND = Path(__file__).resolve().parents[1]
calls = []


def invoke(name, args):
    return (f'<｜｜DSML｜｜ invoke name="{name}">'
            '<｜｜DSML｜｜ parameter name="args" string="false">'
            + json.dumps(args) + '\\</｜｜DSML｜｜ parameter>\\</｜｜DSML｜｜ invoke>')


class Provider(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text = body['messages'][-1]['content']
        calls.append(text)
        if len(calls) == 1:
            answer = ('<｜｜DSML｜｜ calls>' + invoke('list_dir', {'path': '.'})
                      + invoke('file_read', {'path': 'source.txt'})
                      + invoke('file_write', {'path': 'note.txt', 'content': 'hello'})
                      + '</｜｜DSML｜｜ calls>')
        else:
            assert 'SOURCE_EVIDENCE_DSML' in text, text
            answer = 'Completed tools; note.txt contains hello. Source: SOURCE_EVIDENCE_DSML.'
        data = json.dumps({'choices': [{'message': {'role': 'assistant', 'content': answer},
                                       'finish_reason': 'stop'}]}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def main():
    provider = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
    threading.Thread(target=provider.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='coa-dsml-') as tmp:
            state = Path(tmp)
            (state / 'source.txt').write_text('SOURCE_EVIDENCE_DSML', encoding='utf-8')
            (state / 'cognitive-os-agent.json').write_text(json.dumps({
                'llm.provider': 'openai', 'llm.api_key': 'local-test-only',
                'llm.model': 'dsml-fixture', 'reasoning.max_rounds': 5,
                'llm.base_url': f'http://127.0.0.1:{provider.server_port}'}), encoding='utf-8')
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]

            def request(path, body=None):
                req = urllib.request.Request(f'http://127.0.0.1:{port}' + path,
                    data=json.dumps(body).encode() if body is not None else None,
                    headers={'Content-Type': 'application/json'})
                with urllib.request.urlopen(req, timeout=5) as response:
                    return json.load(response)

            binary = BACKEND / 'build' / ('cognitive-os-agent.exe' if __import__('os').name == 'nt' else 'cognitive-os-agent')
            with (state / 'runtime.log').open('w') as log:
                process = subprocess.Popen([str(binary), '--state', str(state), '--workspace', str(state),
                    'serve', str(port)], cwd=BACKEND, stdout=log, stderr=log,
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                try:
                    deadline = time.monotonic() + 20
                    while True:
                        try:
                            request('/v1/scheduler')
                            break
                        except (OSError, ValueError):
                            assert process.poll() is None and time.monotonic() < deadline
                            time.sleep(.05)
                    queued = request('/v1/chat', {'message': 'Read source.txt and create note.txt with hello.'})
                    deadline = time.monotonic() + 20
                    while True:
                        task = request('/v1/tasks/' + str(queued['id']))
                        if task['status'] in ('DONE', 'FAILED', 'CANCELLED', 'TIMEOUT'):
                            break
                        assert time.monotonic() < deadline, task
                        time.sleep(.05)
                    assert task['status'] == 'DONE', task
                    assert task['tool_calls'] == 3 and len(task['steps']) == 3, task
                    assert all(step['ok'] for step in task['steps']), task
                    assert [s['tool'] for s in task['steps']] == ['list_dir', 'file_read', 'file_write'], task
                    assert (state / 'note.txt').read_text() == 'hello'
                    assert len(calls) == 2, len(calls)
                    print('PASS: DSML three-call batch -> real tools -> source evidence -> exact file -> DONE')
                finally:
                    process.terminate()
                    process.wait(timeout=10)
    finally:
        provider.shutdown()
        provider.server_close()


if __name__ == '__main__':
    main()
