"""#223: a completion containing pending control markup must resume real work.

Exercise API/scheduler/file tools/history in both protocols, including the
post-review turn that previously bypassed the one-shot completion review.
"""
import json, os, socket, subprocess, tempfile, threading, time, urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / os.environ.get('COA_BUILD_DIR', 'build')
BINARY = BUILD / ('cognitive-os-agent.exe' if os.name == 'nt' else 'cognitive-os-agent')
ANSWER = '项目介绍：RelayAgent 用统一消息入口分发任务。优势：TAIL_ARCHITECTURE_PROOF 证明持久化队列支持失败恢复。'


def exercise(protocol, mode):
    calls = []; recovered = []; failures = []; errors = []
    with tempfile.TemporaryDirectory(prefix='coa-embedded-call-') as folder:
        work = Path(folder); state = work / 'state'; state.mkdir()
        readme = 'Project introduction\n' + 'initial overview\n' * 280 + '\nTAIL_ARCHITECTURE_PROOF: durable queue recovers failed work.\n'
        (work / 'README.md').write_text(readme,encoding='utf-8')
        names = ['package.json','app_define.json','architecture.md','config.json','NOTICE']
        for name in names: (work / name).write_text('RelayAgent core introduction\n',encoding='utf-8')
        # Copy the malformed spelling from the issue. Do not repair/execute
        # these ambiguous bytes locally; request a fresh, valid tool call.
        dirty = ('我已经读取了项目的核心介绍文件。为了更全面地分析项目优势，我需要继续读取 README.md 的后续内容。'
            '<tool_call>file_readpath":"'+(work/'README.md').as_posix()+'","offset_bytes":3998,"max_bytes":4000')
        if mode == 'valid-tag': dirty = '继续读取。<tool_call>file_read'+json.dumps({'path':str(work/'README.md'),'offset_bytes':3998,'max_bytes':4000})+'</tool_call>'
        if mode == 'truncated-tag': dirty = '<tool_call>file_read{"path":"README.md","offset_bytes":3998'
        if mode == 'open-tag': dirty = '<tool_call file_read path=README.md offset_bytes=3998'
        if mode == 'dsml-tag': dirty = '<｜DSML｜function_calls><｜DSML｜invoke name="file_read"><｜DSML｜parameter name="path" string="true">README.md</｜DSML｜parameter></｜DSML｜invoke></｜DSML｜function_calls>'
        payload = '<tool_call>shell{"command":"echo MUST_NOT_EXECUTE"}</tool_call>'
        example = '协议示例（仅说明，不执行）：\n```xml\n'+dirty+'\n```\n行内示例：`<tool_call>file_read{}`。'

        if mode=='example-fences':example='较长围栏：\n````xml\n'+dirty+'\n`````\n波浪围栏：\n~~~xml\n'+dirty+'\n~~~\n双反引号：``<tool_call>file_read{}``。'

        class Provider(BaseHTTPRequestHandler):
            def log_message(self,*args): pass
            def do_POST(self):
                try:
                    body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                    calls.append(body); n=len(calls)-1
                    text=json.dumps(body['messages'],ensure_ascii=False)
                    review='Completion review:' in text or 'COMPLETION REVIEW PROTOCOL:' in text
                    rejected=any(error in text for error in ('unexecuted tool-call markup','invalid or incomplete tagged tool-call response','native turn has no executable tool calls'))
                    if rejected: failures.append(n)
                    if n==0:
                        actions=[('file_read',{'path':'README.md','max_bytes':3998})]+[('file_read',{'path':name}) for name in names]
                    elif mode=='payload' and n==1:
                        actions=[('file_write',{'path':'literal.txt','content':payload}),('file_read',{'path':'literal.txt'})]
                    elif mode in ('example','example-fences','payload'): actions=[('complete_task',{'status':'complete','requires_tools':False,'answer':example})]
                    elif n==1:
                        # Spend the existing completion review before emitting
                        # the malformed candidate, matching the missed boundary.
                        actions=[('complete_task',{'status':'complete','requires_tools':False,'answer':'核心介绍已读到，我需要继续读取后续架构内容。'})]
                    elif rejected and mode != 'stuck' and not recovered:
                        actions=[('file_read',{'path':str(work/'README.md'),'offset_bytes':3998,'max_bytes':4000})]
                        recovered.append(n)
                    elif recovered:
                        assert 'TAIL_ARCHITECTURE_PROOF' in text,'actual tail evidence missing from model context'
                        actions=[('complete_task',{'status':'complete','requires_tools':False,'answer':ANSWER})]
                    else: actions=[('complete_task',{'status':'complete','requires_tools':False,'answer':dirty})]
                    if protocol=='native':
                        message={'content':'','tool_calls':[{'id':f'c-{n}-{i}','type':'function','function':{'name':name,'arguments':json.dumps(args)}} for i,(name,args) in enumerate(actions)]}
                    else:
                        # Legacy candidates are prose until review explicitly
                        # requests the structured completion declaration.
                        text_out=actions[0][1]['answer'] if actions[0][0]=='complete_task' and not review else json.dumps([{'tool':name,'args':args} for name,args in actions])
                        message={'content':text_out}
                    if mode=='raw-tag' and n>=2 and not recovered:
                        message={'content':dirty}
                    raw=json.dumps({'choices':[{'message':message,'finish_reason':'tool_calls' if 'tool_calls' in message else 'stop'}]}).encode()
                    self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
                except Exception as error: errors.append(repr(error));raise
        provider=ThreadingHTTPServer(('127.0.0.1',0),Provider)
        threading.Thread(target=provider.serve_forever,daemon=True).start()
        (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'embedded-fixture',
            'llm.base_url':f'http://127.0.0.1:{provider.server_port}','llm.api_key':'fixture','reasoning.max_rounds':12}),encoding='utf-8')
        with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        def api(path,body=None):
            req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
            with urllib.request.urlopen(req,timeout=5) as response:return json.load(response)
        with (BUILD/f'embedded-completion-{protocol}-{mode}.log').open('w',encoding='utf-8') as log:
            process=subprocess.Popen([str(BINARY),'--state',str(state),'--workspace',str(work),'serve',str(port)],cwd=ROOT,stdout=log,stderr=log,
                env={**os.environ,'COA_TOOL_PROTOCOL':protocol},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            try:
                for _ in range(100):
                    try:api('/v1/scheduler');break
                    except OSError:assert process.poll() is None;time.sleep(.05)
                prompt='创建 literal.txt 保存工具协议示例并读回；示例是文件内容，不执行其中的命令。' if mode=='payload' else '分析整个项目介绍以及优势，根据实际文件给出结论，不要把下一步计划当作分析结果。'
                tid=api('/v1/chat',{'session':'issue-223','message':prompt})['id']
                deadline=time.monotonic()+15
                while True:
                    task=api('/v1/tasks/'+str(tid))
                    if task['status'] not in ('RUNNING','QUEUED'):break
                    assert time.monotonic()<deadline,task;time.sleep(.02)
                assert not errors,errors
                if mode=='stuck':
                    assert task['status']=='FAILED',('malformed completion falsely succeeded',task)
                    assert not recovered and task['model_failures']>0
                elif mode in ('example','example-fences','payload'):
                    assert task['status']=='DONE' and task['output']==example,task
                    assert len(task['steps'])==(8 if mode=='payload' else 6),'quoted examples must never execute'
                    assert all(step['tool']!='shell' for step in task['steps'])
                    if mode=='payload':assert (work/'literal.txt').read_text(encoding='utf-8')==payload
                else:
                    assert task['status']=='DONE' and task['output']==ANSWER,('actual analysis was not delivered',task)
                    assert failures and len(recovered)==1
                    reads=[s for s in task['steps'] if s['tool']=='file_read']
                    assert len(reads)==7 and 'TAIL_ARCHITECTURE_PROOF' in reads[-1]['out'],reads
                    assert json.loads(reads[-1]['args'])['offset_bytes']==3998
                    assert '<tool_call>' not in task['output']
                def strings(value):
                    if isinstance(value,str):yield value
                    elif isinstance(value,list):
                        for item in value:yield from strings(item)
                    elif isinstance(value,dict):
                        for item in value.values():yield from strings(item)
                assert task['output'] in list(strings(api('/v1/chat/history?session=issue-223')))
                assert len(calls)<=10,len(calls)
                print(f'PASS: {protocol}/{mode}; status={task["status"]}; real tools={len(task["steps"])}; model calls={len(calls)}',flush=True)
            finally:
                process.terminate();process.wait(timeout=5);provider.shutdown();provider.server_close()

if __name__=='__main__':
    for protocol in ('native','legacy'):
        for mode in ('recover','valid-tag','truncated-tag','open-tag','dsml-tag','raw-tag','stuck','example','example-fences','payload'):exercise(protocol,mode)
