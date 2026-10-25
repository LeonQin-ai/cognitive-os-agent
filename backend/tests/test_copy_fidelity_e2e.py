"""Exact bytes and structured completion checks through a real native Runtime."""
import json,os,socket,subprocess,tempfile,threading,time,urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
root=Path(__file__).resolve().parents[1];build=root/os.environ.get('COA_BUILD_DIR','build')
class Model(BaseHTTPRequestHandler):
    calls=0;mode='copy'
    def log_message(self,*args):pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        n=Model.calls;Model.calls+=1
        if n==0:
            name='file_write';args={'path':'target.bin','copy_from':'source.bin'} if Model.mode in ('copy','ask-copy') else {'path':'target.bin','content':'WRONG\n'}
            if Model.mode=='read-guard':name='complete_task';args={'status':'complete','requires_tools':False,'answer':'Equal.','file_equalities':[{'source':'source.bin','target':'target.bin'}]}
            if Model.mode=='missing':args={'path':'target.bin','copy_from':'absent.bin'}
            if Model.mode=='ambiguous':args={'path':'target.bin','copy_from':'source.bin','content':'ambiguous'}
        elif Model.mode=='repair' and n==2:
            name='file_write';args={'path':'target.bin','copy_from':'source.bin'}
        else:
            name='complete_task';args={'status':'complete','requires_tools':False,'answer':'All requested bytes are verified.'}
            if n==1 and Model.mode!='transform':args['file_equalities']=[{'source':'source.bin','target':'target.bin'}]
            if n==3:args['file_equalities']=[]
            # Subsequent completions omit the check deliberately: a failed
            # runtime assertion must remain sticky until actual repair.
        raw=json.dumps({'choices':[{'message':{'content':'','tool_calls':[{'id':'c','type':'function','function':{'name':name,'arguments':json.dumps(args)}}]},'finish_reason':'tool_calls'}],'usage':{'prompt_tokens':11,'completion_tokens':3}}).encode()
        self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
provider=ThreadingHTTPServer(('127.0.0.1',0),Model);threading.Thread(target=provider.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory(prefix='coa-copy-fidelity-') as folder:
    work=Path(folder)
    (work/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixture','llm.api_key':'fixture','llm.base_url':f'http://127.0.0.1:{provider.server_port}','reasoning.max_rounds':6}),encoding='utf8')
    with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    def req(method,path,body=None):
        r=urllib.request.Request(f'http://127.0.0.1:{port}'+path,method=method,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(r,timeout=5) as response:return json.load(response)
    def execute(label,mode,data):
        Model.calls=0;Model.mode=mode
        (work/'source.bin').write_bytes(data)
        tid=req('POST','/v1/chat',{'session':label,'message':'Perform the declared fixture operation.','permission_mode':'ask' if mode in ('read-guard','ask-copy') else 'full'})['id']
        approvals=set()
        deadline=time.monotonic()+15
        while True:
            task=req('GET',f'/v1/tasks/{tid}')
            if task.get('approval') and task['approval']['id'] not in approvals:
                a=task['approval'];assert a['tool']=='file_write',a
                req('POST',f'/v1/tasks/{tid}/approval',{'session':label,'id':a['id'],'allow':True});approvals.add(a['id'])
            if task['status'] in ('DONE','FAILED','CANCELLED','TIMEOUT'):return task
            assert time.monotonic()<deadline,task
            time.sleep(.03)
    with (build/'copy-fidelity-e2e.log').open('w',encoding='utf8') as log:
        server=subprocess.Popen([str(build/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',folder,'--workspace',folder,'serve',str(port)],stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            for _ in range(100):
                try:req('GET','/v1/scheduler');break
                except OSError:assert server.poll() is None;time.sleep(.05)
            for label,data in [('no-newline',b'exact'),('lf',b'exact\n'),('crlf',b'exact\r\n'),('empty',b''),('unicode-binary',b'\xef\xbb\xbf'+ '中文\t尾部 '.encode()+b'\x00\xff\n')]:
                task=execute(label,'copy',data);assert task['status']=='DONE',task
                assert task['file_equalities_verified'] and task['file_equalities_checked']==1,task
                assert (work/'target.bin').read_bytes()==data
            failed=execute('wrong-content','mismatch',b'correct')
            assert failed['status']=='FAILED' and 'equality check failed' in failed['output'],failed
            assert all(s['ok']==1 for s in failed['steps']),failed # successful tools are not proof of requested equality
            fixed=execute('corrected-content','repair',b'correct\r\n')
            assert fixed['status']=='DONE' and (work/'target.bin').read_bytes()==b'correct\r\n',fixed
            assert any(json.loads(s['args']).get('copy_from')=='source.bin' for s in fixed['steps'])
            transformed=execute('intentional-different-output','transform',b'input')
            assert transformed['status']=='DONE' and (work/'target.bin').read_bytes()==b'WRONG\n',transformed
            assert transformed['file_equalities_checked']==0,transformed
            (work/'target.bin').write_bytes(b'not disclosed')
            guarded=execute('no-hidden-read','read-guard',b'not disclosed')
            assert guarded['status']=='FAILED' and guarded['file_equalities_verified'] is False,guarded
            approved=execute('approved-copy-read-scope','ask-copy',b'approved exact bytes')
            assert approved['status']=='DONE' and approved['file_equalities_verified'],approved
            for mode in ('missing','ambiguous'):
                (work/'target.bin').write_bytes(b'unchanged')
                failed=execute(mode,mode,b'correct')
                assert failed['status']=='FAILED' and (work/'target.bin').read_bytes()==b'unchanged',failed
            req('POST','/v1/policy/rules',{'tool':'file_read','action':'deny','reason':'fixture read denial'})
            (work/'target.bin').write_bytes(b'policy preserved')
            denied=execute('copy-source-denied','copy',b'restricted input')
            assert denied['status']=='FAILED' and (work/'target.bin').read_bytes()==b'policy preserved',denied
        finally:server.terminate();server.wait(timeout=5)
finally:provider.shutdown();provider.server_close()
print('PASS: exact copy preserves empty/LF/CRLF/Unicode/BOM/NUL/binary; incorrect successful writes cannot complete; omitted checks stay enforced; actual repair completes')
