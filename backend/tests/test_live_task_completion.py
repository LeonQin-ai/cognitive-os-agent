"""Opt-in completion smoke test using a configured real model, in isolated state.

Usage: python tests/test_live_task_completion.py --config /path/to/cognitive-os-agent.json
Does not modify/restart the installed application or print its API key.
"""
import argparse
import json
import socket
import subprocess
import tempfile
import time
import urllib.request
import uuid
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--config', required=True, type=Path)
args = parser.parse_args()
backend = Path(__file__).resolve().parents[1]
original = json.loads(args.config.read_text(encoding='utf-8'))
config = {key: value for key, value in original.items() if key.startswith('llm.')}
if config.get('llm.provider') in (None, 'mock') or not config.get('llm.api_key'):
    raise SystemExit('A configured real provider and API key are required.')
# This is a bounded smoke-test sandbox, not a change to production budgets.
config['reasoning.max_rounds'] = 8
config['scheduler.workers'] = 1
with tempfile.TemporaryDirectory(prefix='coa-live-completion-') as folder:
    state = Path(folder)
    (state/'cognitive-os-agent.json').write_text(json.dumps(config), encoding='utf-8')
    (state/'docs').mkdir()
    cache_marker, disk_marker = uuid.uuid4().hex, uuid.uuid4().hex
    (state/'docs/cache.txt').write_text('CACHE_POLICY: cache stores pages already read. Marker: '+cache_marker, encoding='utf-8')
    (state/'docs/storage.txt').write_text('DISK_POLICY: disk stores originals and versions. Marker: '+disk_marker, encoding='utf-8')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]

    def request(path, body=None):
        req = urllib.request.Request(f'http://127.0.0.1:{port}'+path,
            data=json.dumps(body).encode() if body is not None else None,
            headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(req, timeout=5) as reply:
            return json.load(reply)

    with (state/'server.log').open('w', encoding='utf-8') as log:
        binary = backend/'build'/('cognitive-os-agent.exe' if __import__('os').name == 'nt' else 'cognitive-os-agent')
        process = subprocess.Popen([str(binary), '--state', str(state), '--workspace', str(state), 'serve', str(port)],
            cwd=backend, stdout=log, stderr=log, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        try:
            for _ in range(100):
                try:
                    request('/v1/config/llm')
                    break
                except OSError:
                    if process.poll() is not None: raise RuntimeError('Test agent exited during startup')
                    time.sleep(.1)
            prompt = ('分析 docs/cache.txt 和 docs/storage.txt 中的缓存与磁盘职责。'
                      '先浏览 docs 目录，再读取两份文件后给出简洁的技术结论，最终答复必须包含每份文件中的完整 Marker。'
                      '只做分析，不要修改文件。不要只说明接下来准备做什么。')
            task_id = request('/v1/tasks', {'prompt':prompt})['id']
            deadline = time.monotonic()+120
            while time.monotonic() < deadline:
                task = request('/v1/tasks/'+str(task_id))
                if task.get('status') not in ('RUNNING','QUEUED'): break
                time.sleep(.5)
            else: raise RuntimeError('Live completion smoke timed out; isolated process will be stopped')
            output = task.get('output') or ''
            summary = {key: task[key] for key in ('status','round','rounds','model_requests','tool_calls','usage','progress') if key in task}
            summary['model'] = config.get('llm.model')
            summary['both_markers_present'] = cache_marker in output and disk_marker in output
            (backend/'build/live-completion-summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
            print(json.dumps(summary, ensure_ascii=False))
            assert task.get('status') == 'DONE', 'Task failed to complete with the configured model'
            assert summary['both_markers_present'], 'Task returned before delivering both file observations'
            print('PASS: real configured API completed directory -> two file reads -> verified final analysis.')
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
