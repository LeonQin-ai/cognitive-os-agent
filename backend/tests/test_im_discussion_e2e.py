"""Real HTTP coverage for bounded, reciprocal IM Agent discussions.

The local OpenAI fixture checks what each role actually receives. No external
model, paid key, desktop UI, or production state is used.
"""
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import urllib.error
import urllib.request


BACKEND = Path(__file__).resolve().parents[1]
BINARY = (BACKEND / os.environ.get('COA_BUILD_DIR', 'build') /
          ('cognitive-os-agent.exe' if os.name == 'nt' else 'cognitive-os-agent')).resolve()
TERMINAL = ('DONE', 'FAILED', 'CANCELLED', 'TIMEOUT')
DONE = '[[DISCUSSION_DONE]]'


class Fixture:
    def __init__(self):
        self.calls = []
        self.errors = []
        self.lock = threading.Lock()
        self.entered = {case: threading.Event() for case in ('CANCEL', 'OLD', 'DELETE')}
        self.release = {case: threading.Event() for case in self.entered}

    def calls_for(self, case):
        with self.lock:
            return [call.copy() for call in self.calls if call['case'] == case]

    def answer(self, text):
        # A role's reasoning session can contain previous rounds. Select the
        # current prompt rather than a role or case mentioned in its history.
        prompt = text[text.rfind('You are '):]
        role_match = re.search(r'You are (alpha|beta) in an IM group', prompt)
        assert role_match, f'Missing role identity: {text[-1800:]}'
        role = role_match.group(1)
        round_match = re.search(r'Discussion round: (\d+)/(\d+)', prompt)
        assert round_match, f'Missing discussion round: {prompt[-1800:]}'
        round_no, rounds = map(int, round_match.groups())
        human = prompt.rsplit('## 用户任务\n', 1)[-1]
        case_match = re.search(r'CASE_([A-Z]+)', human)
        assert case_match, f'Missing original human task: {human}'
        case = case_match.group(1)
        transcript_text = prompt.split('Transcript: ', 1)[1]
        transcript, _ = json.JSONDecoder().raw_decode(transcript_text)
        assert isinstance(transcript, list), transcript
        with self.lock:
            self.calls.append({'case': case, 'role': role, 'round': round_no,
                               'rounds': rounds, 'transcript': transcript})
        contents = '\n'.join(message['content'] for message in transcript)
        if case in self.entered and round_no == 1 and role == 'alpha':
            self.entered[case].set()
            assert self.release[case].wait(20), f'{case} fixture was not released'
            return f'{case}_STALE: this delayed reply must never be appended.', 200
        if case == 'FAIL' and role == 'beta':
            return {'error': {'message': 'Deliberate local Agent failure', 'type': 'fixture'}}, 400
        if case == 'DISCUSS':
            if role == 'alpha' and round_no == 1:
                return 'ALPHA_PROPOSAL: use a staged design with an explicit rollback.', 200
            if role == 'beta' and round_no == 1:
                assert 'ALPHA_PROPOSAL' in contents, transcript
                return 'BETA_QUESTION: how will you verify the rollback?', 200
            if role == 'alpha' and round_no == 2:
                assert 'BETA_QUESTION' in contents, transcript
                return 'ALPHA_RESPONSE: beta, verify rollback with a simulated failure.', 200
            if role == 'beta' and round_no == 2:
                assert 'ALPHA_RESPONSE' in contents, transcript
                return 'BETA_RESPONSE: alpha, the simulated failure addresses my question.', 200
            if role == 'alpha' and round_no == 3:
                assert 'BETA_RESPONSE' in contents, transcript
                return 'ALPHA_FINAL: the shared design includes that verification.', 200
            assert role == 'beta' and round_no == 3, (role, round_no)
            assert 'ALPHA_FINAL' in contents, transcript
            return 'BETA_FINAL: the remaining tradeoff is test isolation.', 200
        if case == 'SINGLE':
            # These are discussion subjects, not requests to perform mutations.
            return ('我的职责是讨论注册、保存和写入方案。' if role == 'alpha' else
                    '我的职责是讨论这些方案的验收标准。'), 200
        if case == 'CONSENSUS':
            return f'{role.upper()}_AGREED: the discussion is complete. {DONE}', 200
        if case == 'PARTIAL':
            marker = ' ' + DONE if role == 'alpha' else ''
            return f'{role.upper()}_PARTIAL_{round_no}: another concern remains.{marker}', 200
        if case == 'NEW':
            assert 'CASE_OLD' in contents, transcript
            assert 'OLD_STALE' not in contents, transcript
            assert 'OLD_STALE' not in text, 'Superseded output leaked into role history or shared memory'
        return f'{role.upper()}_{case}_{round_no}: a concrete discussion contribution.', 200


def make_provider(fixture):
    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_POST(self):
            try:
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                answer, status = fixture.answer(body['messages'][-1]['content'])
                result = ({'usage': {'prompt_tokens': 17, 'completion_tokens': 3}, 'choices': [{'message': {'role': 'assistant', 'content': answer},
                                       'finish_reason': 'stop'}]} if status == 200 else answer)
            except Exception as error:
                with fixture.lock:
                    fixture.errors.append(repr(error))
                status, result = 500, {'error': str(error)}
            data = json.dumps(result).encode('utf-8')
            try:
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                # A cancelled inference can close its socket before the fixture
                # releases. That is an expected outcome of cancellation.
                pass
    return ThreadingHTTPServer(('127.0.0.1', 0), Provider)


def main():
    fixture = Fixture()
    provider = make_provider(fixture)
    threading.Thread(target=provider.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='coa-im-discussion-') as folder:
            state = Path(folder)
            (state / 'im').mkdir()
            (state / 'im/sessions.json').write_text(json.dumps({'next_id': 2, 'sessions': [
                {'id': 1, 'name': 'legacy', 'kind': 'group', 'members': ['alpha', 'beta'],
                 'messages': []}]}), encoding='utf-8')
            (state / 'cognitive-os-agent.json').write_text(json.dumps({
                'llm.provider': 'openai', 'llm.model': 'discussion-local-fixture',
                'llm.api_key': 'local-test-only', 'reasoning.max_rounds': 2,
                'llm.base_url': f'http://127.0.0.1:{provider.server_port}',
                'scheduler.workers': 3,
            }), encoding='utf-8')
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]
            process = None

            def request(method, path, body=None, status=200):
                req = urllib.request.Request(f'http://127.0.0.1:{port}' + path, method=method,
                    data=json.dumps(body).encode('utf-8') if body is not None else None,
                    headers={'Content-Type': 'application/json'})
                try:
                    response = urllib.request.urlopen(req, timeout=5)
                except urllib.error.HTTPError as error:
                    response = error
                with response:
                    result = json.load(response)
                    assert response.code == status, (method, path, response.code, result)
                    return result

            def wait_for(read, ready, label, timeout=30):
                deadline = time.monotonic() + timeout
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

            def finished(task_id):
                assert task_id >= 0, task_id
                return wait_for(lambda: request('GET', f'/v1/tasks/{task_id}'),
                    lambda task: task['status'] in TERMINAL, f'task {task_id}')

            def sessions():
                return request('GET', '/v1/im/sessions')['sessions']

            def room(rounds=None):
                body = {'name': 'discussion', 'kind': 'group', 'members': ['alpha', 'beta']}
                if rounds is not None:
                    body['discussion_rounds'] = rounds
                return request('POST', '/v1/im/sessions', body)['id']

            def send(room_id, case):
                return request('POST', f'/v1/im/sessions/{room_id}/messages', {
                    'content': f'CASE_{case}: Discuss the design and answer each other.',
                    'sender': 'host'})['task_id']

            def messages(room_id):
                return request('GET', f'/v1/im/sessions/{room_id}/messages')

            def replies(room_id):
                return [message for message in messages(room_id) if message['role'] == 'assistant']

            def expect_calls(case, rounds):
                actual = fixture.calls_for(case)
                expected = [(role, number) for number in range(1, rounds + 1)
                            for role in ('alpha', 'beta')]
                assert [(call['role'], call['round']) for call in actual] == expected, actual

            with (state / 'runtime.log').open('w', encoding='utf-8') as log:
                def start():
                    nonlocal process
                    process = subprocess.Popen([str(BINARY), '--state', str(state),
                        '--workspace', str(state), 'serve', str(port)], cwd=state,
                        stdout=log, stderr=log,
                        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                    wait_for(lambda: request('GET', '/v1/scheduler'), lambda _: True, 'startup')

                def stop():
                    if process is not None and process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=5)

                try:
                    start()
                    for name, role in [('alpha', 'Architect'), ('beta', 'Critical reviewer')]:
                        request('POST', '/v1/agents', {'name': name, 'role': role})
                    assert next(s for s in sessions() if s['id'] == 1)['discussion_rounds'] == 3

                    discussion = room()
                    task = finished(send(discussion, 'DISCUSS'))
                    assert task['status'] == 'DONE', task
                    assert task['discussion_round'] == task['discussion_rounds'] == 3, task
                    assert task['llm_calls'] == 6, task
                    assert task['tokens_in'] == 6 * 17 and task['tokens_out'] == 6 * 3, task
                    assert task['usage_available'] and task['model_failures'] == 0, task
                    expect_calls('DISCUSS', 3)
                    assert [m['sender'] for m in replies(discussion)] == ['alpha', 'beta'] * 3
                    print('PASS: two roles exchange proposals, questions and answers across three rounds', flush=True)

                    single = room(1)
                    task = finished(send(single, 'SINGLE'))
                    assert task['status'] == 'DONE', task
                    expect_calls('SINGLE', 1)
                    assert len(replies(single)) == 2
                    assert '注册' in fixture.calls_for('SINGLE')[1]['transcript'][-1]['content']
                    print('PASS: one-round compatibility and discussion text does not trigger mutation retries', flush=True)

                    consensus = room()
                    task = finished(send(consensus, 'CONSENSUS'))
                    assert task['status'] == 'DONE', task
                    expect_calls('CONSENSUS', 1)
                    assert len(replies(consensus)) == 2
                    assert all(DONE not in reply['content'] for reply in replies(consensus))
                    partial = room()
                    assert finished(send(partial, 'PARTIAL'))['status'] == 'DONE'
                    expect_calls('PARTIAL', 3)
                    assert len(replies(partial)) == 6
                    print('PASS: unanimous completion ends early; a single Agent cannot end the discussion', flush=True)

                    failing = room()
                    task = finished(send(failing, 'FAIL'))
                    assert task['status'] == 'FAILED', task
                    assert task['llm_calls'] == len(fixture.calls_for('FAIL')), task
                    assert task['model_failures'] > 0 and task['tokens_in'] == 17 and task['tokens_out'] == 3, task
                    assert task['discussion_round'] == 1, task
                    assert any(reply['sender'] == 'alpha' for reply in replies(failing)), messages(failing)
                    assert all(reply['sender'] != 'beta' for reply in replies(failing)), messages(failing)
                    print('PASS: a failed Agent fails the task while retaining completed contributions', flush=True)

                    cancelled = room()
                    task_id = send(cancelled, 'CANCEL')
                    assert fixture.entered['CANCEL'].wait(10), 'Cancellation fixture was not called'
                    request('DELETE', f'/v1/tasks/{task_id}')
                    fixture.release['CANCEL'].set()
                    task = finished(task_id)
                    assert task['status'] == 'CANCELLED', task
                    assert task['discussion_round'] == 1, task
                    assert replies(cancelled) == [], messages(cancelled)
                    assert len(fixture.calls_for('CANCEL')) == 1
                    print('PASS: cancelling an in-flight model call discards its eventual reply', flush=True)

                    superseded = room()
                    old_id = send(superseded, 'OLD')
                    assert fixture.entered['OLD'].wait(10), 'Superseded fixture was not called'
                    new_id = send(superseded, 'NEW')
                    fixture.release['OLD'].set()
                    old = finished(old_id)
                    assert old['status'] == 'CANCELLED' and old['discussion_round'] == 1, old
                    assert finished(new_id)['status'] == 'DONE'
                    expect_calls('NEW', 3)
                    assert len(fixture.calls_for('OLD')) == 1
                    assert len(replies(superseded)) == 6
                    assert all('OLD_STALE' not in reply['content'] for reply in replies(superseded))
                    print('PASS: a new human message takes over and stale Agent output is discarded', flush=True)

                    deleted = room()
                    deleted_id = send(deleted, 'DELETE')
                    assert fixture.entered['DELETE'].wait(10), 'Deleted-room fixture was not called'
                    request('DELETE', f'/v1/im/sessions/{deleted}')
                    fixture.release['DELETE'].set()
                    assert finished(deleted_id)['status'] in ('CANCELLED', 'FAILED')
                    assert len(fixture.calls_for('DELETE')) == 1
                    assert not any(s['id'] == deleted for s in sessions())
                    print('PASS: deleting a room prevents further model turns and message appends', flush=True)

                    configurable = room(5)
                    for invalid in (0, 6, -1, 1.5, True, False, None, '2', [], {}):
                        body = {'name': 'invalid', 'kind': 'group', 'members': ['alpha', 'beta'],
                                'discussion_rounds': invalid}
                        request('POST', '/v1/im/sessions', body, 400)
                        request('PUT', f'/v1/im/sessions/{configurable}/members', {
                            'members': ['alpha', 'beta'], 'discussion_rounds': invalid}, 400)
                        assert next(s for s in sessions() if s['id'] == configurable)['discussion_rounds'] == 5
                    request('PUT', f'/v1/im/sessions/{configurable}/members', {
                        'members': ['alpha', 'beta'], 'discussion_rounds': 2})
                    request('PUT', f'/v1/im/sessions/{configurable}/members', {'members': ['beta', 'alpha']})
                    updated = next(s for s in sessions() if s['id'] == configurable)
                    assert updated['discussion_rounds'] == 2 and updated['members'] == ['beta', 'alpha'], updated

                    direct = request('POST', '/v1/im/sessions', {'name': 'direct'})['id']
                    before = len(fixture.calls)
                    direct_result = request('POST', f'/v1/im/sessions/{direct}/messages', {
                        'content': 'Direct messages do not start a group discussion.'})
                    assert direct_result['task_id'] < 0, direct_result
                    assert len(messages(direct)) == 1
                    assert len(fixture.calls) == before
                    print('PASS: strict round limits, atomic invalid settings and direct-message compatibility', flush=True)

                    snapshot = messages(discussion)
                    stop()
                    start()
                    restored = next(s for s in sessions() if s['id'] == configurable)
                    assert restored['discussion_rounds'] == 2 and restored['members'] == ['beta', 'alpha'], restored
                    assert messages(discussion) == snapshot
                    assert not fixture.errors, fixture.errors
                    print('PASS: discussion settings, member order and reciprocal messages survive restart', flush=True)
                except Exception:
                    log.flush()
                    print((state / 'runtime.log').read_text(encoding='utf-8', errors='replace')[-10000:], flush=True)
                    raise
                finally:
                    for release in fixture.release.values():
                        release.set()
                    stop()
    finally:
        provider.shutdown()
        provider.server_close()


if __name__ == '__main__':
    main()
