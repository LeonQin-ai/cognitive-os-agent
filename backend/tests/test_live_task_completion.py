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
parser.add_argument('--scoped-reading', action='store_true', help='Require EOF for cache.txt but only a fragment of storage.txt')
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
    cache_text='CACHE_POLICY: cache stores pages already read. Marker: '+cache_marker
    disk_text='DISK_POLICY: disk stores originals and versions. Marker: '+disk_marker
    if args.scoped_reading:
        cache_text=('cache source page\n'*480)+cache_text
        disk_text=disk_text+'\n'+('optional appendix\n'*2000)
    (state/'docs/cache.txt').write_text(cache_text, encoding='utf-8')
    (state/'docs/storage.txt').write_text(disk_text, encoding='utf-8')
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
            if args.scoped_reading:
                prompt=('完整阅读文件 docs/cache.txt；docs/storage.txt 只需读取开头的相关片段，不要求全文，不要翻页读取附录。'
                        '根据两份文件解释缓存与磁盘职责。最终答复必须包含两份文件的完整 Marker，'
                        '不要修改文件。完成规定范围的读取和分析后交付结果。')
            task_id = request('/v1/tasks', {'prompt':prompt})['id']
            deadline = time.monotonic()+120
            while time.monotonic() < deadline:
                task = request('/v1/tasks/'+str(task_id))
                if task.get('status') not in ('RUNNING','QUEUED'): break
                time.sleep(.5)
            else: raise RuntimeError('Live completion smoke timed out; isolated process will be stopped')
            output = task.get('output') or ''
            (backend/'build/live-completion-task.json').write_text(json.dumps(task,ensure_ascii=False,indent=2),encoding='utf-8')
            summary = {key: task[key] for key in ('status','round','llm_calls','tool_calls','model_failures','tokens_in','tokens_out','usage_available') if key in task}
            summary['model'] = config.get('llm.model')
            summary['both_markers_present'] = cache_marker in output and disk_marker in output
            if args.scoped_reading:
                steps=task.get('steps',[])
                disk_reads=[json.loads(step['args']) for step in steps if step.get('tool')=='file_read' and
                    Path(json.loads(step['args']).get('path','')).name=='storage.txt']
                summary['fragment_offsets']=[read.get('offset_bytes',0) for read in disk_reads]
            (backend/'build/live-completion-summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding='utf-8')
            print(json.dumps(summary, ensure_ascii=False))
            if task.get('status') != 'DONE': print((task.get('output') or '')[:700])
            assert task.get('status') == 'DONE', 'Task failed to complete with the configured model'
            assert summary['both_markers_present'], 'Task returned before delivering both file observations'
            if args.scoped_reading:
                assert summary['fragment_offsets'] and not any(summary['fragment_offsets']), 'Optional appendix was incorrectly promoted to mandatory EOF'
            print('PASS: real configured API completed directory -> two file reads -> verified final analysis.')
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
