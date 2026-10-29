"""HTTP regressions: ten roles are candidates, not ten mandatory steps."""
import json,os,socket,subprocess,tempfile,threading,time,urllib.request,urllib.error
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
root=Path(__file__).resolve().parents[1]
class Model(BaseHTTPRequestHandler):
    choice='role3';calls=0;prompts=[]
    def log_message(self,*args):pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        Model.calls+=1;Model.prompts.append(body)
        content=json.dumps([{'agent':Model.choice,'task':'inspect source','after':[]}])
        raw=json.dumps({'choices':[{'message':{'content':content},'finish_reason':'stop'}]}).encode()
        self.send_response(200);self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
server=ThreadingHTTPServer(('127.0.0.1',0),Model)
threading.Thread(target=server.serve_forever,daemon=True).start()
try:
 with tempfile.TemporaryDirectory() as folder:
    state=Path(folder)
    (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'fixture','llm.api_key':'fixture','llm.base_url':f'http://127.0.0.1:{server.server_port}'}))
    with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    def api(path,body=None,status=200):
        request=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
        try:response=urllib.request.urlopen(request,timeout=10)
        except urllib.error.HTTPError as error:response=error
        assert response.status==status,(response.status,response.read())
        return json.load(response)
    with (root/'build/flow-agent-selection.log').open('w') as log:
     proc=subprocess.Popen([str(root/'build/cognitive-os-agent.exe' if os.name=='nt' else root/'build/cognitive-os-agent'),'--state',folder,'--workspace',folder,'serve',str(port)],cwd=root,stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
     try:
        for _ in range(100):
            try:api('/v1/scheduler');break
            except OSError:time.sleep(.1)
        for i in range(10):api('/v1/agents',{'name':f'role{i}','role':f'role description {i}'})
        plan=api('/v1/flows/decompose',{'task':'inspect source','agents':['role3']})['dag']
        assert len(plan['nodes'])==1 and plan['nodes'][0]['agent']=='role3'
        prompt=json.dumps(Model.prompts[-1]);assert 'role3 (role description 3)' in prompt and 'role4 (role description 4)' not in prompt
        Model.choice='role4';api('/v1/flows/decompose',{'task':'inspect','agents':['role3']},422)
        def wait_failed(tid):
            deadline=time.monotonic()+15
            while True:
                result=api(f'/v1/tasks/{tid}')
                if result['status'] not in ('RUNNING','QUEUED'):break
                assert time.monotonic()<deadline;time.sleep(.05)
            assert result['status']=='FAILED' and not result.get('tool_calls'),result
        tid=api('/v1/orchestrate',{'task':'inspect','agents':['role3']})['id']
        wait_failed(tid)
        record=next(x for x in api('/v1/flows/list')['records'] if x['id']==tid)
        assert json.loads(record['input'])['agents']==['role3']
        resumed=api(f'/v1/flows/{tid}/resume',{})['id']
        wait_failed(resumed)
        before=Model.calls
        for agents in ([],['missing'],'role3'):
            api('/v1/flows/decompose',{'task':'inspect','agents':agents},400)
            api('/v1/orchestrate',{'task':'inspect','agents':agents},400)
        assert Model.calls==before
        plan=api('/v1/flows/decompose',{'task':'inspect'})['dag']
        assert len(plan['nodes'])==1 and plan['nodes'][0]['agent']=='role4'
        print('PASS: ten-role auto subset, selected-only roster, out-of-scope atomic rejection, invalid selection before model call.')
     finally:proc.terminate();proc.wait(timeout=10)
finally:server.shutdown();server.server_close()
