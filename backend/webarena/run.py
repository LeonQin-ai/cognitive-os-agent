# run.py — WebArena-style mini eval for cognitive-os-agent: real browser tasks
# on stable sandbox sites (books.toscrape.com / quotes.toscrape.com) driven by
# the Playwright MCP server, scored against programmatically verified ground
# truths (probe of the live pages at harness-authoring time). Tasks run
# sequentially: one shared Playwright browser must not be driven concurrently.
# Resumable via results.jsonl.
#
# usage: python run.py [--port 18600]
import json
import os
import re
import shutil
import string
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BACKEND = os.path.dirname(HERE)
WORK = os.path.join(BACKEND, 'webarena-run')
INSTALL_CFG = ('C:/Users/94207/AppData/Local/Temp/opencode/'
               'cagent-install-test/state/cognitive-os-agent.json')
RESULTS = os.path.join(HERE, 'results.jsonl')
PORT = 18600
TASK_TIMEOUT_S = 1800
POLL = 3


def norm(s):
    s = s.lower()
    s = ''.join(' ' if c in string.punctuation else c for c in s)
    words = [w for w in s.split() if w not in ('a', 'an', 'the')]
    return ' '.join(words)


def numeric_eq(a, b):
    try:
        return float(a) == float(b)
    except (ValueError, TypeError):
        return False


def match(answer, expect):
    # Strict: these 6 tasks have deterministic short answers, so only exact
    # (normalized) or numeric equality counts — no substring fallback, which
    # could credit refusals that merely mention the value.
    if not answer:
        return False
    ne = norm(expect)
    na = norm(answer.strip().splitlines()[-1] if answer.strip() else '')
    if na == ne:
        return True
    return numeric_eq(na, ne)


def http_json(method, path, body=None, timeout=30):
    url = f'http://127.0.0.1:{PORT}{path}'
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def setup():
    if os.path.exists(WORK):
        shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(os.path.join(WORK, 'state'))
    cfg = json.load(open(INSTALL_CFG, encoding='utf-8'))
    cfg['scheduler.workers'] = 1
    with open(os.path.join(WORK, 'state', 'cognitive-os-agent.json'), 'w',
              encoding='utf-8') as f:
        json.dump(cfg, f, indent=1)
    log = open(os.path.join(WORK, 'server.log'), 'ab')
    exe = os.path.join(BACKEND, 'build', 'cognitive-os-agent.exe')
    env = dict(os.environ)
    pydir = os.path.dirname(os.path.abspath(sys.executable))
    env['PATH'] = pydir + os.pathsep + env.get('PATH', '')
    proc = subprocess.Popen([exe, 'serve', str(PORT)], cwd=WORK,
                            env=env, stdout=log, stderr=log)
    for _ in range(120):
        try:
            http_json('GET', '/v1/blackboard')
            return proc
        except Exception:
            time.sleep(1)
    raise RuntimeError('server did not come up; see webarena-run/server.log')


def register_playwright():
    r = http_json('POST', '/v1/mcp', {
        'name': 'playwright',
        'transport': 'stdio',
        'command': 'node',
        'args': os.path.join(HERE, 'node_modules', '@playwright', 'mcp', 'cli.js'),
    })
    print('mcp register:', json.dumps(r)[:200], flush=True)


def run_task(t):
    prompt = (t['question'] +
              '\n\nWhen done, end your reply with a line "ANSWER: <value>".')
    try:
        r = http_json('POST', '/v1/orchestrate', {'task': prompt})
    except Exception as e:
        return {'task_id': t['task_id'], 'status': 'SUBMIT_FAIL',
                'got': '', 'ok': False, 'err': str(e)}
    tid = r.get('id')
    t0 = time.time()
    while time.time() - t0 < TASK_TIMEOUT_S:
        try:
            st = http_json('GET', f'/v1/tasks/{tid}')
        except Exception as e:
            st = {'status': 'POLL_ERR', 'err': str(e)}
        s = st.get('status', '?')
        if s not in ('QUEUED', 'RUNNING', 'POLL_ERR'):
            out = st.get('output') or ''
            m = re.search(r'ANSWER:\s*(.+)', out)
            final = m.group(1).strip() if m else out
            return {'task_id': t['task_id'], 'status': s, 'got': out,
                    'final': final[:200], 'ok': s == 'DONE' and match(final, t['answer']),
                    'expect': t['answer'], 'ms': int((time.time() - t0) * 1000)}
        time.sleep(POLL)
    return {'task_id': t['task_id'], 'status': 'HARNESS_TIMEOUT',
            'got': '', 'ok': False, 'expect': t['answer']}


def report(done):
    rows = sorted(done.values(), key=lambda r: r['task_id'])
    n = len(rows)
    ok = sum(1 for r in rows if r.get('ok'))
    print(f'\n== WebArena-style mini (n={n}): {ok}/{n} = {100.0*ok/n:.0f}% ==',
          flush=True)
    for r in rows:
        mark = 'PASS' if r.get('ok') else 'FAIL'
        print(f"  [{mark}] {r['task_id']} status={r['status']} "
              f"final={r.get('final', '')[:80]!r}", flush=True)
    with open(os.path.join(HERE, 'final_scores.json'), 'w',
              encoding='utf-8') as f:
        json.dump(rows, f, indent=1, ensure_ascii=False)


def main():
    tasks = json.load(open(os.path.join(HERE, 'tasks.json'), encoding='utf-8'))
    done = {}
    if os.path.exists(RESULTS):
        for line in open(RESULTS, encoding='utf-8'):
            line = line.strip()
            if line:
                r = json.loads(line)
                cur = done.get(r['task_id'])
                if cur is None or (cur['status'] != 'DONE' and r['status'] == 'DONE'):
                    done[r['task_id']] = r
    todo = [t for t in tasks if t['task_id'] not in done]
    print(f'WebArena-style mini: {len(todo)} to do ({len(done)} done)', flush=True)

    proc = setup()
    try:
        register_playwright()
        if not todo:
            report(done)
            return
        for t in todo:
            print(f"=== {t['task_id']} ===", flush=True)
            res = run_task(t)
            print(f"  status={res['status']} ok={res.get('ok')} "
                  f"final={res.get('final', '')[:100]!r}", flush=True)
            with open(RESULTS, 'a', encoding='utf-8') as f:
                f.write(json.dumps(res, ensure_ascii=False) + '\n')
            done[t['task_id']] = res
        report(done)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    main()
