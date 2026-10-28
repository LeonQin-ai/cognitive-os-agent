"""#232 subcase: new work invalidates the prior delivery review in both protocols."""
import json,os,socket,subprocess,tempfile,threading,time,urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from fixture_http_response import respond_json

root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
candidate='已获得入口线索。\n\n还有采样路径需要核实。'
phase=('我需要继续分析 sysctl.c 中 cpufreq 相关的完整逻辑。'
       '从已读内容看到关键线索：新内核使用 scaling_cur_freq 而非 cpuinfo_cur_freq。\n\n'
       '我将同时读取后续内容并搜索关键函数定义。')
answer='已核实：FIRST_SOURCE、SECOND_SOURCE、FINAL_SOURCE 均来自实际读取。新内核选择 scaling_cur_freq，硬件采样读取 cpuinfo_cur_freq，采样来源及时间不同导致差异。'
class Provider(BaseHTTPRequestHandler):
    protocol='native';calls=0;reviews=0
    def log_message(self,*_):pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));n=Provider.calls;Provider.calls+=1
        Provider.reviews+='Completion review:' in json.dumps(body,ensure_ascii=False)
        name,args='complete_task',{'status':'complete','requires_tools':False,'answer':candidate if n==1 else phase if n==3 else answer}
        if n in (0,2,4):name,args='file_read',{'path':f'source{n//2}.c'}
        if n>=5:assert 'FINAL_SOURCE' in json.dumps(body),'final delivery skipped actual source'
        if self.protocol=='native':
            message={'content':'','tool_calls':[{'id':f'call-{n}','type':'function','function':{'name':name,'arguments':json.dumps(args)}}]}
        else:
            message={'content':candidate if n==1 else json.dumps([{'tool':name,'args':args}],ensure_ascii=False)}
        respond_json(self,json.dumps({'choices':[{'message':message,'finish_reason':'tool_calls' if self.protocol=='native' else 'stop'}]}).encode())

for protocol in ('legacy','native'):
    Provider.protocol=protocol;Provider.calls=0;Provider.reviews=0
    server=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=server.serve_forever,daemon=True).start()
    try:
      with tempfile.TemporaryDirectory(prefix='coa-resumed-review-') as folder:
        state=Path(folder)
        for i,marker in enumerate(('FIRST_SOURCE','SECOND_SOURCE','FINAL_SOURCE')):(state/f'source{i}.c').write_text(marker,encoding='utf8')
        (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixture','llm.api_key':'fixture',
          'llm.base_url':f'http://127.0.0.1:{server.server_port}','reasoning.max_rounds':12}),encoding='utf8')
        with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
        def api(path,body=None):
            req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
            with urllib.request.urlopen(req,timeout=5) as response:return json.load(response)
        with (build/f'resumed-review-{protocol}.log').open('w',encoding='utf8') as log:
          proc=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',folder,'--workspace',folder,'serve',str(port)],cwd=root,stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':protocol},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
          try:
            for _ in range(150):
                try:api('/v1/scheduler');break
                except OSError:assert proc.poll() is None;time.sleep(.05)
            tid=api('/v1/chat',{'message':'分析三个源码文件中的 CPU 频率采样路径与差异，给出完整结论。','session':'resumed-review'})['id']
            end=time.monotonic()+20
            while True:
                task=api(f'/v1/tasks/{tid}')
                if task['status'] not in ('RUNNING','QUEUED'):break
                assert time.monotonic()<end;time.sleep(.03)
            assert task['status']=='DONE' and task['output']==answer,{'status':task['status'],'output':task['output'],'calls':Provider.calls}
            assert task['tool_calls']==3 and Provider.calls==7,(task['tool_calls'],Provider.calls)
            assert task['tool_protocol']==protocol and task['completion_declaration']=='complete'
            assert api('/v1/chat/history?session=resumed-review')['turns'][-1]['a']==answer
            for _ in range(100):
                records=api('/v1/tasks/journal');completed=[row for row in records if row.get('id')==tid and row.get('status')=='DONE']
                if completed:break
                time.sleep(.02)
            assert completed and any(event.get('tool_protocol')==protocol and event.get('completion_declaration')=='complete' for event in completed[-1].get('trace',[])), 'journal lost protocol/completion metadata'
            print(f'PASS #232 {protocol}: resumed work invalidates old review, final source read and independently reviewed delivery retained.',flush=True)
          finally:proc.terminate();proc.wait(timeout=8)
    finally:server.shutdown();server.server_close()
