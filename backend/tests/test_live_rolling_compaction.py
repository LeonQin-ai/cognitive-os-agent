"""Opt-in real Runtime compaction check; no provider calls or fabricated history.

25 genuine chat tasks trigger two compactions of the same session. Memory
sharing is disabled in a private Runtime so episode recall cannot mask a lost
rolling summary. The 500-turn deterministic interface test is separate.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.request
import uuid

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(__doc__)
parser.add_argument('--config', required=True, type=Path)
parser.add_argument('--report', type=Path, default=root/'build/live-rolling-compaction.json')
parser.add_argument('--max-total-tokens', type=int, default=200000)
parser.add_argument('--preload-user-inputs', action='store_true', help='target compaction with 15 synthetic user-only inputs, no fabricated assistant answers; then execute 10 real tasks')
parser.add_argument('--focus-retention', action='store_true', help='progress asks for brief acknowledgement without exact ACK formatting; final random constraint recall remains exact')
args = parser.parse_args()
if args.max_total_tokens <= 0:
    parser.error('max-total-tokens must be positive')
config = json.loads(args.config.read_text(encoding='utf-8-sig'))
config.update({'auth.key': '', 'http.bind': '127.0.0.1', 'reasoning.max_rounds': 5, 'llm.timeout_ms': 60000})
binary = root/'build'/('cognitive-os-agent.exe' if os.name == 'nt' else 'cognitive-os-agent')
report = {'schema': 1, 'model': config.get('llm.model'), 'cases': [], 'passed': False,
          'scope': ('15 synthetic user-only inputs + 10 real native chat tasks' if args.preload_user_inputs else '25 actual native Runtime chat tasks')+' / two rolling compactions; not a 500-turn LLM certification'}
report['focus_retention']=args.focus_retention
args.report.parent.mkdir(parents=True, exist_ok=True)

def save():
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')

with tempfile.TemporaryDirectory(prefix='coa-live-rolling-') as folder:
    state = Path(folder); work = state/'workspace'; work.mkdir()
    (state/'cognitive-os-agent.json').write_text(json.dumps(config), encoding='utf-8')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]
    def api(method, path, body=None):
        request = urllib.request.Request(f'http://127.0.0.1:{port}'+path, method=method,
            data=json.dumps(body).encode() if body is not None else None,
            headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=10) as response:
            return json.load(response)
    with args.report.with_suffix('.log').open('w', encoding='utf-8') as log:
        process = subprocess.Popen([str(binary), '--state', str(state), '--workspace', str(work), 'serve', str(port)],
            cwd=root, stdout=log, stderr=log, env={**os.environ, 'COA_TOOL_PROTOCOL': 'native'},
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        session = None
        try:
            for _ in range(150):
                try:
                    api('GET', '/v1/scheduler'); break
                except OSError:
                    assert process.poll() is None, 'private Runtime exited'
                    time.sleep(.1)
            else:
                raise TimeoutError('private Runtime not ready')
            session = api('POST', '/v1/chat/sessions', {})['id']
            api('POST', f'/v1/chat/sessions/{session}/shared', {'shared': False})
            forbidden = 'protected_'+uuid.uuid4().hex[:12]
            acceptance = 'verify_'+uuid.uuid4().hex[:12]
            expected = forbidden+'|'+acceptance
            def task(label, message, expected_answer):
                result = {}; started = time.monotonic()
                tid = api('POST', '/v1/chat', {'session': session, 'message': message, 'permission_mode': 'ask'})['id']
                error = None
                try:
                    while True:
                        result = api('GET', f'/v1/tasks/{tid}')
                        if result['status'] not in ('RUNNING', 'QUEUED'):
                            break
                        if time.monotonic()-started > 120:
                            api('DELETE', f'/v1/tasks/{tid}')
                            for _ in range(40):
                                result = api('GET', f'/v1/tasks/{tid}')
                                if result['status'] not in ('RUNNING', 'QUEUED'):
                                    break
                                time.sleep(.2)
                            raise TimeoutError('private task cancelled after 120s')
                        time.sleep(.2)
                except Exception as exc:
                    error = type(exc).__name__
                row = {k: result.get(k) for k in ('status', 'output', 'llm_calls', 'tool_calls', 'tokens_in',
                    'tokens_out', 'model_failures', 'model_failure', 'usage_available')}
                row.update(case=label, id=tid, seconds=round(time.monotonic()-started, 3), expected=expected_answer,
                    passed=not error and result.get('status') == 'DONE' and (bool(result.get('output', '').strip()) if expected_answer is None else result.get('output', '').strip() == expected_answer))
                row['efficiency_passed'] = row['passed'] and row.get('usage_available') is True and not row.get('model_failures') and (row.get('tokens_out') or 0) <= 4096
                if error:
                    row['error'] = error
                report['cases'].append(row)
                # This ledger includes compaction requests, not just planner calls.
                report['usage'] = api('GET', '/v1/usage')
                save()
                print(json.dumps({k: v for k, v in row.items() if k not in ('output', 'expected')}, ensure_ascii=False), flush=True)
                assert row['passed'], f'case {label} failed; retained in report'
                assert row['efficiency_passed'], f'case {label} had a model failure or excessive output; no automatic retry'
                total = report['usage'].get('total', {})
                assert total.get('prompt', 0)+total.get('completion', 0)+total.get('reasoning', 0) <= args.max_total_tokens, 'aggregate test budget reached; no automatic retry'
            initial = f'本会话一直有效的约束：禁止修改目录 {forbidden}；任务最终必须执行验收命令 {acceptance}。这是虚构测试，不执行任何工具。请记住，只回答 ACK。'
            if args.preload_user_inputs:
                # Only synthetic user inputs are seeded. All compressor output
                # and delivered answers must come from the configured model.
                inputs = [initial]+[f'历史用户进度 {i}：讨论日志排版，无新增或撤销约束。' for i in range(1, 15)]
                transcript = state/'chat'/f'{session}.jsonl'
                transcript.write_text(''.join(json.dumps({'q': q, 'a': ''}, ensure_ascii=False)+'\n' for q in inputs), encoding='utf-8')
                assert len(api('GET', '/v1/chat/history?session='+session)['turns']) == 15
                report['synthetic_user_inputs'] = 15
            else:
                task('initial-constraints', initial, 'ACK')
            for turn in range(15 if args.preload_user_inputs else 1, 24):
                suffix='简短确认收到即可。' if args.focus_retention else '只回答 ACK。'
                task(f'progress-{turn}', f'普通进度记录 {turn}：本轮讨论测试数据分组与日志排版，没有新增或撤销约束，不操作文件、不调用工具，'+suffix, None if args.focus_retention else 'ACK')
            task('recall-after-second-compaction', '最初约束中禁止修改的目录名称与最终验收命令分别是什么？只输出 目录名称|验收命令，不调用工具；没有记录则只输出 UNKNOWN。', expected)
            history = api('GET', '/v1/chat/history?session='+session)
            turns = history.get('turns', []) if isinstance(history, dict) else history
            report['history_turns_after'] = len(turns)
            # 24th push must have compacted again: exactly eight retained turns,
            # then the final recall creates the ninth. Mere prompt recall is insufficient.
            assert len(turns) == 9, f'expected two compactions and nine recent turns, got {len(turns)}'
            report['passed'] = True
        except Exception as error:
            report['error'] = str(error)
        finally:
            try:
                report['usage'] = api('GET', '/v1/usage')
                if session is not None:
                    history = api('GET', '/v1/chat/history?session='+session)
                    report['history_turns_after'] = len(history.get('turns', []))
            except OSError:
                pass
            recall = next((row for row in report['cases'] if row['case']=='recall-after-second-compaction'), {})
            report['retention_passed'] = bool(recall.get('passed')) and report.get('history_turns_after') == 9
            report['efficiency_passed'] = bool(recall) and all(row.get('efficiency_passed') for row in report['cases'])
            process.terminate(); process.wait(timeout=10)
            report['usage_complete'] = bool(report['cases']) and all(row.get('usage_available') is True for row in report['cases'])
            save()
print(json.dumps({k: v for k, v in report.items() if k != 'cases'}, ensure_ascii=False))
raise SystemExit(0 if report['passed'] else 1)
