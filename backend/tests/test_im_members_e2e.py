"""Group selection validation, repair of legacy empty rooms, replies and restart."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

BACKEND = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix='coa-im-members-') as tmp:
        state = Path(tmp)
        (state / 'im').mkdir()
        (state / 'im/sessions.json').write_text(json.dumps({'next_id': 2, 'sessions': [
            {'id': 1, 'name': 'legacy-empty', 'kind': 'group', 'members': [], 'messages': []}]}))
        (state / 'cognitive-os-agent.json').write_text(json.dumps({'llm.provider': 'mock',
            'llm.model': 'mock', 'scheduler.workers': 2}))
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]

        def request(method, path, body=None, status=200):
            req = urllib.request.Request(f'http://127.0.0.1:{port}' + path, method=method,
                data=json.dumps(body).encode() if body is not None else None,
                headers={'Content-Type': 'application/json'})
            try:
                response = urllib.request.urlopen(req, timeout=5)
            except urllib.error.HTTPError as error:
                response = error
            with response:
                data = json.load(response)
                assert response.code == status, (method, path, response.code, data)
                return data

        binary = BACKEND / 'build' / ('cognitive-os-agent.exe' if os.name == 'nt' else 'cognitive-os-agent')
        with (state / 'runtime.log').open('w') as log:
            def start():
                process = subprocess.Popen([str(binary), '--state', str(state), '--workspace', str(state),
                    'serve', str(port)], stdout=log, stderr=log, cwd=BACKEND,
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                deadline = time.monotonic() + 20
                while True:
                    try:
                        request('GET', '/v1/scheduler')
                        return process
                    except (OSError, ValueError):
                        assert process.poll() is None and time.monotonic() < deadline
                        time.sleep(.05)

            process = start()
            try:
                request('POST', '/v1/im/sessions/1/messages', {'content': 'hello'}, 409)
                assert request('GET', '/v1/im/sessions/1/messages') == []
                for name in ['alpha', 'beta']:
                    request('POST', '/v1/agents', {'name': name, 'role': 'Reviewer'})
                for members in [None, [], ['ghost'], ['alpha', 'alpha'], [1], ['alpha'] * 65]:
                    request('POST', '/v1/im/sessions', {'name': 'invalid', 'kind': 'group', 'members': members}, 400)
                    request('PUT', '/v1/im/sessions/1/members', {'members': members}, 400)
                assert request('GET', '/v1/im/sessions')['sessions'][0]['members'] == []
                request('PUT', '/v1/im/sessions/999/members', {'members': ['alpha']}, 404)
                direct = request('POST', '/v1/im/sessions', {'name': 'direct'})['id']
                request('PUT', f'/v1/im/sessions/{direct}/members', {'members': ['alpha']}, 400)
                room = request('POST', '/v1/im/sessions', {'name': 'new', 'kind': 'group', 'members': ['beta']})['id']
                assert room > 1
                request('PUT', '/v1/im/sessions/1/members', {'members': ['alpha', 'beta']})
                queued = request('POST', '/v1/im/sessions/1/messages', {'content': 'Say hello briefly.'})
                assert queued['task_id'] >= 0, queued
                deadline = time.monotonic() + 20
                while True:
                    task = request('GET', '/v1/tasks/' + str(queued['task_id']))
                    if task['status'] in ('DONE', 'FAILED', 'CANCELLED', 'TIMEOUT'):
                        break
                    assert time.monotonic() < deadline, task
                    time.sleep(.05)
                assert task['status'] == 'DONE', task
                replies = request('GET', '/v1/im/sessions/1/messages')
                assert [m['sender'] for m in replies if m['role'] == 'assistant'] == ['alpha', 'beta'], replies
                process.terminate()
                process.wait(timeout=10)
                process = start()
                sessions = request('GET', '/v1/im/sessions')['sessions']
                assert next(s for s in sessions if s['id'] == 1)['members'] == ['alpha', 'beta']
                assert len(request('GET', '/v1/im/sessions/1/messages')) == 3
                request('DELETE', '/v1/agents/alpha')
                request('POST', '/v1/im/sessions/1/messages', {'content': 'keep this unsent'}, 409)
                assert len(request('GET', '/v1/im/sessions/1/messages')) == 3
                print('PASS: empty/unknown/duplicate group rejection, legacy repair, two-agent replies, persistence and stale roster')
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)


if __name__ == '__main__':
    main()
