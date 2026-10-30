"""Actionable incomplete diagnostics and real task resume regression (#128)."""
import json, os, socket, subprocess, tempfile, threading, time, urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from fixture_completion import reviewed_answer
counts={}
class Provider(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text=body['messages'][-1]['content']
        current=text.split('## Current request\n',1)[-1].split('\n## ',1)[0]
        if 'MALFORMED180' in current:
            n=counts.get('bad180',0);counts['bad180']=n+1
            answer=('<tool_call>file_read\",\"args\":{\"path\":\"index180.html\"}</tool_call>' if n==0 else
                    json.dumps([{'tool':'file_write','args':{'path':'index180.html','content':'<html>verified</html>'}}]) if n==1 else
                    json.dumps([{'tool':'file_read','args':{'path':'index180.html'}}]) if n==2 else
                    '报告已生成并核对：index180.html。')
        elif 'HEADER_BAD178' in current:
            n=counts.get('headerbad178',0);counts['headerbad178']=n+1
            answer=(json.dumps([{'tool':'file_read','args':{'path':'src/context/example.h'}}]) if n==0 else
                    json.dumps([{'tool':'file_read','args':{'path':'other/include/context/example.h'}}]) if n==1 else
                    '分析已经完成。')
        elif 'HEADER178' in current:
            n=counts.get('header178',0);counts['header178']=n+1
            answer=(json.dumps([{'tool':'file_read','args':{'path':'src/context/example.h'}}]) if n==0 else
                    '分析已完成。' if n==1 else
                    json.dumps([{'tool':'file_read','args':{'path':'include/context/example.h'}}]) if n==2 else
                    '分析结果：头文件已从 include/context/example.h 读取并核实。')
            if n==2: assert 'src/context/example.h' in text.replace('\\','/'),text[-1500:]
        elif 'HTML177' in current:
            n=counts.get('html177',0);counts['html177']=n+1
            answer=(json.dumps([{'tool':'list_dir','args':{'path':'.'}}]) if n==0 else
                    '根据之前的分析，我现在需要创建一个 HTML 报告文档，整理上下文工程的架构和流程图。我已经有完整的分析内容，直接生成 HTML 文件。' if n==1 else
                    json.dumps([{'tool':'file_write','args':{'path':'report177.html','content':'<!doctype html><html><body><svg></svg>Context architecture and flow</body></html>'}},{'tool':'file_read','args':{'path':'report177.html'}}]) if n==2 else
                    '报告已交付：report177.html，包含架构和流程图。')
        elif 'HTML_FALSE177' in current: answer='报告已经完成。'
        elif 'USE_TOOLS176' in current:
            n=counts.get('tools176',0);counts['tools176']=n+1
            answer=(json.dumps([{'tool':'list_dir','args':{'path':'.'}}]) if n==0 else
                    '根据已有初步资料，我需要调用工具完成检查，再交付实际分析。' if n==1 else
                    json.dumps([{'tool':'list_dir','args':{'path':'.'}}]) if n==2 else
                    '检查结果：目录资料已核对，现有分析已完成。')
        elif 'ANALYSIS_175' in current:
            n=counts.get('analysis175',0);counts['analysis175']=n+1
            answer=(json.dumps([{'tool':'file_read','args':{'path':'partial.txt','max_bytes':100}}]) if n==0 else
                    '我已经阅读了大部分核心文件。让我读取 `mmu.c` 的剩余部分，以完成对上下文工程实现的完整分析。' if n==1 else
                    json.dumps([{'tool':'file_read','args':{'path':'partial.txt','offset_bytes':7500}}]) if n==2 else
                    '上下文工程分析结果：已经读取到 EOF_DIAGNOSTIC；缓存负责已读页面复用，磁盘负责原文持久化。')
        elif 'HTML_STALL' in current:
            n=counts.get('stall',0);counts['stall']=n+1
            answer='我需要先检查报告内容，然后继续生成架构图。阶段 '+str(n)
        elif 'HTML_RECOVERY' in current:
            n=counts.get('html',0); counts['html']=n+1
            answer=(json.dumps([{'tool':'file_write','args':{'path':'report.html','content':'<!doctype html><html><body>Architecture and flow diagram</body></html>'}}]) if n==0 else
                    '我需要先检查已生成的报告文件是否存在且内容完整。' if n==1 else
                    '让我读取现有的 HTML 报告文件，确认架构图和流程图。' if n==2 else
                    json.dumps([{'tool':'file_read','args':{'path':'report.html'}}]) if n==3 else
                    '报告已生成并读取核对：report.html。')
        elif 'UTF8_DIAGNOSTIC' in current:
            n=counts.get('utf8',0); counts['utf8']=n+1
            answer=(json.dumps([{'tool':'shell','args':{'command':'python emit_invalid.py'}}]) if n==0 else
                    json.dumps([{'tool':'shell','args':{'command':'python emit_valid.py'}}]) if n==1 else
                    '编码记录已核对，采样完成。')
        elif 'REGISTER_DIAGNOSTIC' in current: answer='技能已经注册。'
        else:
            n=counts.get('read',0); counts['read']=n+1
            if n==0: answer=json.dumps([{'tool':'file_read','args':{'path':'partial.txt'}}])
            elif counts.get('recover') and n==1:
                answer=json.dumps([{'tool':'file_read','args':{'path':'partial.txt','offset_bytes':4000}}])
            elif counts.get('recover'):
                assert 'EOF_DIAGNOSTIC' in text; answer='完整读取完成，已核对 EOF_DIAGNOSTIC。'
            else: answer='全部阅读完成。'
        answer=reviewed_answer(body,answer)
        data=json.dumps({'choices':[{'message':{'content':answer}}]}).encode()
        self.send_response(200); self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
backend = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coa-completion-recovery-') as folder:
    state = Path(folder)
    provider = ThreadingHTTPServer(('127.0.0.1',0), Provider)
    threading.Thread(target=provider.serve_forever,daemon=True).start()
    (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'skill-test',
        'llm.base_url':f'http://127.0.0.1:{provider.server_port}','reasoning.max_rounds':6}),encoding='utf-8')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0)); port = sock.getsockname()[1]
    def request(path, body=None):
        req = urllib.request.Request(f'http://127.0.0.1:{port}'+path,
            data=json.dumps(body).encode() if body is not None else None,
            headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(req,timeout=5) as r: return json.load(r)
    with (backend/'build/completion-diagnostics-e2e.log').open('w',encoding='utf-8') as log:
        process = subprocess.Popen([str(backend/'build/cognitive-os-agent.exe'),'--state',str(state),
            '--workspace',str(state),'serve',str(port)],cwd=backend,stdout=log,stderr=log,
            env={**os.environ,'COA_TOOL_PROTOCOL':'legacy'},
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            deadline = time.monotonic()+20
            while True:
                try: request('/v1/scheduler'); break
                except OSError:
                    assert process.poll() is None and time.monotonic()<deadline
                    time.sleep(.1)
            def finished(task_id):
                deadline=time.monotonic()+20
                while True:
                    t=request('/v1/tasks/'+str(task_id))
                    if t['status'] in ('DONE','FAILED','CANCELLED','TIMEOUT'): return t
                    assert time.monotonic()<deadline,t
                    time.sleep(.05)
            repaired=finished(request('/v1/tasks',{'prompt':'生成HTML文档 MALFORMED180'})['id'])
            assert repaired['status']=='DONE' and counts['bad180']==5,repaired
            assert [st['tool'] for st in repaired['steps']]==['file_write','file_read'],repaired
            print('PASS: malformed tagged identifier never dispatches; corrected write/read completes without phantom failure')
            (state/'include/context').mkdir(parents=True)
            (state/'include/context/example.h').write_text('HEADER178',encoding='utf-8')
            header=finished(request('/v1/tasks',{'prompt':'分析头文件 HEADER178'})['id'])
            assert header['status']=='DONE' and counts['header178']==5,header
            print('PASS: missing src header scope is retained; verified same-repository include read recovers')
            (state/'other/include/context').mkdir(parents=True)
            (state/'other/include/context/example.h').write_text('unrelated repository',encoding='utf-8')
            header_bad=finished(request('/v1/tasks',{'prompt':'分析头文件 HEADER_BAD178'})['id'])
            assert header_bad['status']=='FAILED',header_bad
            print('PASS: same header basename in a different repository does not clear failed reads')
            artifact=finished(request('/v1/tasks',{'prompt':'输出一个html的报告文档，整理对应的架构和流程图 HTML177'})['id'])
            assert artifact['status']=='DONE' and counts['html177']==5 and (state/'report177.html').exists(),artifact
            false_report=finished(request('/v1/tasks',{'prompt':'输出一个html的报告文档 HTML_FALSE177'})['id'])
            assert false_report['status']=='FAILED',false_report
            print('PASS: issue177 requires an actual HTML artifact; no-write completion is rejected')
            tool_needed=finished(request('/v1/tasks',{'prompt':'分析当前架构 USE_TOOLS176'})['id'])
            assert tool_needed['status']=='DONE' and counts['tools176']==4,tool_needed
            print('PASS: explicit remaining tool need after observations continues tool execution')
            html=finished(request('/v1/tasks',{'prompt':'输出一个html报告并检查内容 HTML_RECOVERY'})['id'])
            assert html['status']=='DONE' and counts.get('html')==6,html
            assert (state/'report.html').exists()
            assert any(step.get('tool')=='file_read' for step in html.get('steps',[])),html
            print('PASS: HTML write followed by two narration rounds recovers to actual read and final delivery')
            stalled=finished(request('/v1/tasks',{'prompt':'输出HTML报告 HTML_STALL'})['id'])
            # Physical artifact rejection stops three invalid completion attempts.
            assert stalled['status']=='FAILED' and counts['stall']==4,stalled
            print('PASS: narration without any progress stops after bounded recovery; no false DONE')
            no_action=finished(request('/v1/tasks',{'prompt':'注册这些skills REGISTER_DIAGNOSTIC'})['id'])
            assert no_action['status']=='FAILED' and 'skill_manage' in no_action['output'],no_action
            (state/'partial.txt').write_text('x'*7500+'EOF_DIAGNOSTIC',encoding='utf-8')
            analysis=finished(request('/v1/tasks',{'prompt':'分析代码中的上下文工程怎么实现 ANALYSIS_175'})['id'])
            assert analysis['status']=='DONE' and counts['analysis175']==5,analysis
            assert 'EOF_DIAGNOSTIC' in analysis['output'] and '让我读取' not in analysis['output'],analysis
            print('PASS: issue175 progress preamble is not final analysis; remaining read and actual answer execute')
            started=request('/v1/tasks',{'prompt':'完整读取文件 partial.txt READ_DIAGNOSTIC'})
            partial=finished(started['id'])
            assert partial['status']=='FAILED' and 'offset_bytes=4000' in partial['output'] and 'partial.txt' in partial['output'],partial
            counts['recover']=True; counts['read']=0
            resumed=finished(request('/v1/tasks/'+str(started['id'])+'/resume',{})['id'])
            assert resumed['status']=='DONE' and 'EOF_DIAGNOSTIC' in resumed['output'],resumed
            (state/'emit_invalid.py').write_text('import sys\nsys.stdout.buffer.write(("NORMAL_UNICODE_正常中文"+"中"*1000+"EOF_UTF8_DIAGNOSTIC").encode("utf-8"))\nsys.exit(1)\n',encoding='utf-8')
            (state/'emit_valid.py').write_text('import sys\nsys.stdout.buffer.write("NORMAL_UNICODE_正常中文".encode("utf-8"))\n',encoding='utf-8')
            encoded=finished(request('/v1/tasks',{'prompt':'检查工具输出编码 UTF8_DIAGNOSTIC'})['id'])
            assert encoded['status']=='DONE',encoded
            steps=encoded.get('steps',[])
            assert steps and 'NORMAL_UNICODE_正常中文' in steps[0].get('out','') and 'EOF_UTF8_DIAGNOSTIC' in steps[0].get('out',''),steps
            assert counts.get('utf8')==4,counts
            print('PASS: invalid UTF-8 tool output remains valid task/progress JSON; valid Chinese survives')
            print('PASS: missing registration operation and unread file have actionable causes; resumed read reaches EOF and DONE')
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
            provider.shutdown()
