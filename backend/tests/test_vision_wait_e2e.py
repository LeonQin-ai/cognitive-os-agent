"""#131: real upload / image payload / slow provider / subsequent recovery."""
import base64, json, os, socket, subprocess, tempfile, threading, time
import urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from fixture_completion import reviewed_answer

backend=Path(__file__).resolve().parents[1]
captures=[]
release_silent=threading.Event()
mode='silent'
remaining_lengths=0
produced_actions=0
class Provider(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        captures.append(body)
        images=[p for m in body['messages'] if isinstance(m['content'],list)
                for p in m['content'] if p.get('type')=='image_url']
        if images:
            assert images[0]['image_url']['url'].startswith('data:image/png;base64,')
            assert body['max_tokens']==8192
            if mode=='silent':
                release_silent.wait(30)
                return
        else: assert body['max_tokens']==32768
        if mode in ('length_once','length_tools','length_forever','invalid_response'):
            global remaining_lengths,produced_actions
            if mode=='invalid_response':
                data=json.dumps({'choices':[{'message':{'content':None},'finish_reason':'stop'}]}).encode()
            elif mode=='length_forever' or remaining_lengths:
                remaining_lengths=max(0,remaining_lengths-1)
                data=json.dumps({'choices':[{'message':{'content':None,'reasoning_content':'private reasoning'},'finish_reason':'length'}]}).encode()
            else:
                reply={'content':reviewed_answer(body,'VISION_WAIT_OK')}
                if mode=='length_tools' and produced_actions<2:
                    name,args=(('file_write',{'path':'delivered.txt','content':'VERIFIED_VISUAL_DELIVERY'}) if produced_actions==0 else ('file_read',{'path':'delivered.txt'}))
                    produced_actions+=1
                    if body.get('tools'):
                        reply={'content':'','tool_calls':[{'id':f'visual-{produced_actions}','type':'function','function':{'name':name,'arguments':json.dumps(args)}}]}
                    else:reply={'content':json.dumps([{'tool':name,'args':args}])}
                data=json.dumps({'choices':[{'message':reply,'finish_reason':'tool_calls' if reply.get('tool_calls') else 'stop'}]}).encode()
            self.send_response(200);self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
            return
        answer=reviewed_answer(body,'VISION_WAIT_OK') if mode=='ready' else 'VISION_WAIT_OK'
        data=json.dumps({'choices':[{'message':{'content':answer}}]}).encode()
        self.send_response(200); self.send_header('Content-Length',str(len(data))); self.end_headers()
        try:
            if images and mode=='trickle':
                for byte in data:
                    self.wfile.write(bytes([byte])); self.wfile.flush(); time.sleep(.03)
            else: self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError): pass

with tempfile.TemporaryDirectory(prefix='coa-vision-wait-') as folder:
    state=Path(folder)
    provider=ThreadingHTTPServer(('127.0.0.1',0),Provider)
    threading.Thread(target=provider.serve_forever,daemon=True).start()
    base_url=f'http://127.0.0.1:{provider.server_port}'
    (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'capture-vision',
        'llm.base_url':base_url,'llm.api_key':'local-fixture'}))
    # Set the mock route's declared context window explicitly. The planner
    # allocates at most one quarter of this to output (capped at 32768), while
    # the visual path independently caps output at 8192. Include the legacy
    # field too so this fixture works across both route metadata schemas.
    (state/'routes.json').write_text(json.dumps([{'name':'capture-vision','provider':'openai','model':'capture-vision',
        'base_url':base_url,'api_key':'local-fixture','context_window':131072,
        'context_window_tokens':131072}]))
    with socket.socket() as sock: sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
    def request(path,body=None):
        data=json.dumps(body).encode() if isinstance(body,dict) else body
        with urllib.request.urlopen(urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=data,
            headers={'Content-Type':'application/json'}),timeout=5) as response: return json.load(response)
    binary=backend/os.environ.get('COA_BUILD_DIR','build')/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
    with (binary.parent/'vision-wait-e2e.log').open('w',encoding='utf-8') as log:
        env=os.environ.copy(); env['COA_LLM_VISION_TIMEOUT_MS']='1000'
        process=subprocess.Popen([str(binary),'--state',str(state),'--workspace',str(state),'serve',str(port)],
            cwd=backend,env=env,stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            deadline=time.monotonic()+20
            while True:
                try: request('/v1/scheduler'); break
                except OSError:
                    assert process.poll() is None and time.monotonic()<deadline
                    time.sleep(.05)
            png=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=')
            uploaded=request('/v1/upload?name=image.png&session=vision',png)
            def chat(attachments):
                before=time.monotonic()
                task_id=request('/v1/chat',{'message':'描述附件','session':'vision','attachments':attachments})['id']
                while True:
                    task=request('/v1/tasks/'+str(task_id))
                    if task['status'] in ('DONE','FAILED','TIMEOUT','CANCELLED'): return task,time.monotonic()-before
                    assert time.monotonic()-before<5,task
                    time.sleep(.02)
            failed,elapsed=chat([uploaded['id']])
            assert failed['status']=='FAILED' and 'capture-vision' in failed['output'] and '1000 ms' in failed['output'],failed
            assert elapsed<5,elapsed
            assert len(captures)==1,len(captures)
            mode='trickle'
            failed,elapsed=chat([uploaded['id']])
            assert failed['status']=='FAILED' and '1000 ms' in failed['output'],failed
            assert elapsed<5 and len(captures)==2,(elapsed,len(captures))
            mode='ready'
            done,_=chat([uploaded['id']]); assert done['status']=='DONE',done
            plain,_=chat([]); assert plain['status']=='DONE',plain
            assert len(captures)==4,len(captures)
            # The same HTTP-200 length response used to kill a visual task on
            # its first failure. Both modalities share one bounded recovery.
            for attachments in ([uploaded['id']],[]):
                mode='length_once';remaining_lengths=1;before=len(captures)
                recovered,_=chat(attachments)
                assert recovered['status']=='DONE' and recovered['output']=='VISION_WAIT_OK',recovered
                assert len(captures)-before==2,(len(captures)-before,recovered)
            mode='length_forever';before=len(captures)
            exhausted,_=chat([uploaded['id']])
            assert exhausted['status']=='FAILED' and len(captures)-before==3,exhausted
            assert 'token' in exhausted['output'].lower() and '120000 ms' not in exhausted['output'],exhausted
            mode='invalid_response';before=len(captures)
            invalid,_=chat([uploaded['id']])
            assert invalid['status']=='FAILED' and len(captures)-before==1,invalid
            mode='ready'
            after,_=chat([uploaded['id']]);assert after['status']=='DONE',after
            mode='length_tools';remaining_lengths=1;produced_actions=0
            delivered,_=chat([uploaded['id']])
            assert delivered['status']=='DONE' and (state/'delivered.txt').read_text()=='VERIFIED_VISUAL_DELIVERY',delivered
            assert [s['tool'] for s in delivered['steps']]==['file_write','file_read'],delivered
            assert delivered['model_failures']==1,delivered
            print('PASS: text/image length recovery, bounded repeated length, invalid image response, and real write/readback after recovery')
            print('PASS: permanently silent and trickling providers fail without retry/synthesis; image retry and text succeed; payload and budgets verified')
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
            provider.shutdown()
            release_silent.set()
