"""Exercise completion state through real HTTP, scheduler, planner and file tools."""
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8')

counts = {}
NON_SSH_TEXT = ('password=CodeOnly_98+! token=ghp_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN\n'
    'Bearer abcdef1234567890abcdef123456\n'
    'D:/random-Q7m2V9k4X1r8T6p3Z5n0/backend/src/record.c')
class Provider(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text=body['messages'][-1]['content']
        current=text.split('## Current request\n', 1)[-1].split('\n## ', 1)[0]
        if '已执行动作的观察记录' in text:
            answer='仅完成部分源码读取。'
        elif '限定全文范围回归测试' in current or '必读未完成范围回归测试' in current:
            key='scoped' if '限定全文范围回归测试' in current else 'required-incomplete'
            count=counts.get(key,0);counts[key]=count+1
            if count==0:
                answer=json.dumps([{'tool':'file_read','args':{'path':'source.c'}},
                    {'tool':'file_read','args':{'path':'fragment.c'}}])
            elif key=='scoped' and count==1:
                answer=json.dumps([{'tool':'file_read','args':{'path':'source.c','offset_bytes':4000}}])
            elif key=='scoped' and count==2:
                answer=json.dumps([{'tool':'file_write','args':{'path':'generated.py','content':'g'*7000}},
                    {'tool':'file_read','args':{'path':'generated.py'}}])
            else: answer='只读取了fragment.c的相关片段。source.c全文已核对，分析完成。'
        elif '源码阅读恢复回归测试' in current:
            count=counts.get('recovery', 0); counts['recovery']=count+1
            replies=[
                '[{"tool":"list_dir","args":{"path":"."}}]',
                '[{"tool":"list_dir","args":{"path":"."}}]',
                '[{"tool":"file_read","args":{"path":"source.c"}}]',
                '暂时只读了第一段，尚未读取后续源码。',
                '[{"tool":"file_read","args":{"path":"source.c","offset_bytes":4000}}]',
                '源码阅读完成：已读到 END_SOURCE。']
            answer=replies[min(count, len(replies)-1)]
        elif '批量源码观察回归测试' in current:
            count=counts.get('batch', 0); counts['batch']=count+1
            if count == 0:
                answer=json.dumps([{'tool':'file_read','args':{'path':f'batch-{i}.c'}} for i in range(3)])
            else:
                assert 'MIDDLE_SOURCE_PROOF' in text and 'output elided' not in text
                answer='批量源码完整保留。'
        elif '长答案历史回归测试' in current:
            answer='源码资料：'+'文'*5000+'REPORT_END_MARKER'
        elif '非SSH文本保留回归测试' in current:
            count=counts.get('unredacted',0); counts['unredacted']=count+1
            if count==0:
                answer=json.dumps([{'tool':'file_read','args':{'path':'unredacted.c'}}])
            else:
                assert NON_SSH_TEXT in text and '[REDACTED:secret]' not in text
                answer=NON_SSH_TEXT
        elif '超大分页参数回归测试' in current:
            count=counts.get('capped',0); counts['capped']=count+1
            schema_text=body['messages'][0]['content']
            assert '"maximum":4000' in schema_text and '"required":["path"]}' in schema_text
            if count==0:
                answer=json.dumps([{'tool':'file_read','args':{'path':'source.c','max_bytes':8000}}])
            elif count==1:
                assert 'next_offset_bytes=4000' in text and 'max_bytes capped to 4000' in text
                answer=json.dumps([{'tool':'file_read','args':{'path':'source.c','offset_bytes':4000,'max_bytes':8000}}])
            else:
                assert 'END_SOURCE' in text and 'end of file' in text
                answer='连续分页完成，已读到 END_SOURCE。'
        elif any(case in current for case in ('缺少发现证据回归测试','同名无关文件回归测试','只有路径发现回归测试')):
            key = next(case for case in ('缺少发现证据回归测试','同名无关文件回归测试','只有路径发现回归测试') if case in current)
            count=counts.get(key,0); counts[key]=count+1
            if count==0:
                answer=json.dumps([{'tool':'file_read','args':{'path':'src/memory/kv.c'}}])
            elif count==1 and key!='缺少发现证据回归测试':
                answer=json.dumps([{'tool':'glob','args':{'path':'backend','pattern':'**/kv.c'}}])
            elif key=='缺少发现证据回归测试' and count==1:
                answer=json.dumps([{'tool':'file_read','args':{'path':(state/'backend/src/memory/kv.c').as_posix()}}])
            elif key=='同名无关文件回归测试' and count==2:
                answer=json.dumps([{'tool':'file_read','args':{'path':(state/'backend/src/storage/kv.c').as_posix()}}])
            else: answer='整理已完成。'
        elif '嵌套根目录发现回归测试' in current:
            count=counts.get('discovery',0); counts['discovery']=count+1
            discovered=(state/'backend/src/memory/kv.c').as_posix()
            if count==0:
                answer=json.dumps([{'tool':'file_read','args':{'path':'src/memory/kv.c'}}])
            elif count==1:
                assert 'glob/list_dir' in text and 'workspace' in text
                answer=json.dumps([{'tool':'glob','args':{'path':'backend','pattern':'**/kv.c'}}])
            elif count==2:
                assert discovered in text
                answer=json.dumps([{'tool':'file_read','args':{'path':discovered}}])
            else:
                assert 'NESTED_SOURCE_EVIDENCE' in text
                answer='已基于实际发现的源码整理面试题。'
        else:
            tool='list_dir' if '目录读取回归测试' in current else 'file_read'
            answer=json.dumps([{'tool':tool, 'args':{'path':'.' if tool=='list_dir' else 'readme.txt'}}])
        data=json.dumps({'choices':[{'message':{'role':'assistant','content':answer},
                     'finish_reason':'stop'}]}).encode()
        self.send_response(200); self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)

backend = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coa-source-read-') as directory:
    state = Path(directory)
    (state / 'source.c').write_text('x' * 7970 + 'END_SOURCE' + 'x' * 20, encoding='utf-8')
    (state / 'readme.txt').write_text('source evidence', encoding='utf-8')
    (state/'fragment.c').write_text('partial evidence\n'*2000,encoding='utf-8')
    (state/'backend/src/memory').mkdir(parents=True)
    (state/'backend/src/memory/kv.c').write_text('NESTED_SOURCE_EVIDENCE',encoding='utf-8')
    (state/'backend/src/storage').mkdir(parents=True)
    (state/'backend/src/storage/kv.c').write_text('UNRELATED_SAME_BASENAME',encoding='utf-8')
    (state/'unredacted.c').write_bytes(NON_SSH_TEXT.encode('utf-8'))
    for i in range(3):
        (state / f'batch-{i}.c').write_text('x' * 1900 +
            ('MIDDLE_SOURCE_PROOF' if i == 1 else 'x' * 19) + 'x' * 1981, encoding='utf-8')
    provider=ThreadingHTTPServer(('127.0.0.1',0), Provider)
    threading.Thread(target=provider.serve_forever,daemon=True).start()
    (state / 'cognitive-os-agent.json').write_text(json.dumps({'llm.provider': 'openai',
        'llm.model':'source-read-test', 'llm.api_key':'local-test-only',
        'llm.base_url':f'http://127.0.0.1:{provider.server_port}', 'reasoning.max_rounds': 12}), encoding='utf-8')
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    base = f'http://127.0.0.1:{port}'
    def request(path, body=None):
        req = urllib.request.Request(base+path,
            data=json.dumps(body).encode() if body is not None else None,
            headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, timeout=5) as response:
            return json.load(response)
    with (backend / 'build' / 'source-read-e2e-server.log').open('w') as log:
        process = subprocess.Popen([str(backend / 'build' / 'cognitive-os-agent.exe'),
            '--state', str(state), '--workspace', str(state), 'serve', str(port)],
            cwd=backend, stdout=log, stderr=log, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        try:
            deadline = time.monotonic()+20
            while True:
                try:
                    request('/v1/scheduler'); break
                except (OSError, ValueError):
                    assert time.monotonic() < deadline and process.poll() is None
                    time.sleep(.1)
            cases = [
                ('源码阅读恢复回归测试：完整阅读代码并输出完备资料', 'DONE', '源码阅读完成'),
                ('批量源码观察回归测试：完整阅读代码', 'DONE', '批量源码完整保留'),
                ('重复读取回归测试：完整阅读源码', 'FAILED', '任务未完成'),
                ('目录读取回归测试：完整阅读源码', 'FAILED', '任务未完成'),
                ('长答案历史回归测试：输出详细面试资料', 'DONE', 'REPORT_END_MARKER'),
                ('嵌套根目录发现回归测试：阅读代码整理面试题', 'DONE', '实际发现'),
                ('超大分页参数回归测试：完整阅读源码', 'DONE', '连续分页完成'),
                ('非SSH文本保留回归测试：读取源码并原样输出', 'DONE', 'CodeOnly_98+!'),
                ('缺少发现证据回归测试：阅读代码', 'FAILED', 'file_read'),
                ('同名无关文件回归测试：阅读代码', 'FAILED', 'file_read'),
                ('只有路径发现回归测试：阅读代码', 'FAILED', 'file_read'),
                ('限定全文范围回归测试：完整阅读文件source.c；fragment.c只需相关片段，不要求全文；生成脚本并检查相关片段即可。', 'DONE', '分析完成'),
                ('必读未完成范围回归测试：完整阅读文件source.c；fragment.c只需相关片段，不要求全文。', 'FAILED', 'source.c'),
            ]
            for index, (message, expected, marker) in enumerate(cases):
                started = request('/v1/chat', {'message': message, 'session': f'read-test-{index}'})
                deadline = time.monotonic()+20
                while True:
                    task = request('/v1/tasks/'+str(started['id']))
                    if task['status'] in ('DONE', 'FAILED', 'CANCELLED', 'TIMEOUT'):
                        break
                    assert time.monotonic() < deadline
                    time.sleep(.05)
                assert task['status'] == expected, task
                assert marker in task.get('output', ''), task
                if index == 1:
                    assert any('MIDDLE_SOURCE_PROOF' in step.get('out','') for step in task.get('steps',[])), task
                    for step in task.get('steps',[]):
                        assert json.loads(step['args'])['path'].startswith('batch-')
                if index == 4:
                    long_output=task['output']
                if index == 6:
                    assert task['tool_calls']==2 and all(step['ok']==1 for step in task['steps']), task
                if index == 7:
                    assert task['output']==NON_SSH_TEXT, task
                    assert NON_SSH_TEXT in task['steps'][0]['out'], task
                    saved=request('/v1/chat/history?session=read-test-7')
                    assert saved['turns'][-1]['a']==NON_SSH_TEXT
                if index == 11:
                    assert task['tool_calls']==5 and task['llm_calls']==4, task
                    assert sum(json.loads(step['args']).get('path')=='fragment.c' for step in task['steps'])==1
                    assert sum(step['tool']=='file_read' and json.loads(step['args']).get('path')=='generated.py' for step in task['steps'])==1
                if index == 12:
                    assert 'fragment.c：offset_bytes=' not in task['output'], task
                print('PASS:', expected, message, flush=True)
            history=request('/v1/chat/history?session=read-test-4')
            assert history['turns'][-1]['a']==long_output, 'Chat history truncated the answer'
            process.terminate(); process.wait(timeout=10)
            process=subprocess.Popen([str(backend / 'build' / 'cognitive-os-agent.exe'),
                '--state',str(state),'--workspace',str(state),'serve',str(port)],
                cwd=backend,stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            deadline=time.monotonic()+20
            while True:
                try: restored=request('/v1/chat/history?session=read-test-4'); break
                except OSError:
                    assert time.monotonic()<deadline; time.sleep(.1)
            assert restored['turns'][-1]['a']==long_output, 'Restart truncated the saved answer'
            print('PASS #107: long Unicode answer survives history reload and server restart')
            print(f'PASS: {len(cases)} real API scenarios, no paid model calls.')
        finally:
            process.terminate()
            process.wait(timeout=10)
            provider.shutdown(); provider.server_close()
