"""#241: actual image bytes, read receipts, model vision and scoped previews."""
import base64,json,os,socket,subprocess,tempfile,threading,time,urllib.request,urllib.error
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from fixture_http_response import respond_json
ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/os.environ.get('COA_BUILD_DIR','build')
PNG=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=')
JPEG=base64.b64decode('/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAACAAIDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDi6KKK+ZP3E//Z')
def exercise(protocol,mode):
    calls=[];errors=[];expected='FAILED' if mode=='invalid' else 'DONE'
    data=JPEG if mode=='jpeg' else PNG
    filename='picture.jpg' if mode=='jpeg' else 'picture.png'
    actions=[('file_read',{'path':filename})]
    if mode=='generated':actions.insert(0,('shell',{'command':'python make.py','outputs':[filename]}))
    if mode=='cached':actions.append(('file_read',{'path':filename,'max_bytes':4}))
    actions.append(('complete_task',{'status':'complete','requires_tools':False,'answer':filename}))
    class Provider(BaseHTTPRequestHandler):
        def log_message(self,*_):pass
        def do_POST(self):
            try:
                body=json.loads(self.rfile.read(int(self.headers['Content-Length'])));n=len(calls);calls.append(body)
                if n==0 and mode=='generated':
                    prompt=json.dumps(body.get('system',''))+' '+json.dumps(body['messages'][0]['content'])
                    assert 'image preview' in prompt, 'planner omitted image readback guidance'
                images=[]
                for message in body['messages']:
                    for block in message['content'] if isinstance(message['content'],list) else []:
                        if block.get('type')=='image_url':images.append(block['image_url']['url'].split(',',1)[-1])
                        if block.get('type')=='image':images.append(block['source']['data'])
                after_read=n>=(2 if mode=='generated' else 1)
                if after_read and mode!='invalid':assert images==[base64.b64encode(data).decode()],images
                elif not after_read or mode=='invalid':assert not images,images
                name,args=actions[min(n,len(actions)-1)]
                if protocol=='anthropic':reply={'content':[{'type':'tool_use','id':f'c{n}','name':name,'input':args}],'stop_reason':'tool_use'}
                else:
                    message={'content':json.dumps([{'tool':name,'args':args}])}
                    if protocol=='native':message={'content':'正在读取图片以核实交付内容。','tool_calls':[{'id':f'c{n}','type':'function','function':{'name':name,'arguments':json.dumps(args)}}]}
                    reply={'choices':[{'message':message,'finish_reason':'stop'}]}
                respond_json(self,json.dumps(reply).encode())
            except Exception as e:errors.append(repr(e));self.send_error(500)
    server=ThreadingHTTPServer(('127.0.0.1',0),Provider);threading.Thread(target=server.serve_forever,daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix='coa-image-read-') as folder:
            state=Path(folder)
            if mode!='generated':(state/filename).write_bytes(b'not an image\x00' if mode=='invalid' else data)
            (state/'make.py').write_text("import base64\nopen('picture.png','wb').write(base64.b64decode("+repr(base64.b64encode(PNG).decode())+"))\n")
            (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'anthropic' if protocol=='anthropic' else 'openai','llm.model':'image-fixture','llm.api_key':'fixture','llm.base_url':f'http://127.0.0.1:{server.server_port}','reasoning.max_rounds':8}))
            with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
            def api(path,body=None):
                req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
                with urllib.request.urlopen(req,timeout=5) as response:return json.load(response)
            with (BUILD/f'image-read-{protocol}-{mode}.log').open('w',encoding='utf8') as log:
                proc=subprocess.Popen([str(BUILD/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',folder,'--workspace',folder,'serve',str(port)],cwd=ROOT,env={**os.environ,'COA_TOOL_PROTOCOL':'legacy' if protocol=='legacy' else 'native'},stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                try:
                    for _ in range(150):
                        try:api('/v1/scheduler');break
                        except OSError:assert proc.poll() is None;time.sleep(.05)
                    acceptance={'artifacts':[filename]} if mode=='generated' else {'requires_observation':True}
                    tid=api('/v1/chat',{'message':'Display the image.','session':'image-test','acceptance':acceptance})['id'];deadline=time.monotonic()+30
                    while True:
                        task=api(f'/v1/tasks/{tid}')
                        if task['status'] not in ('RUNNING','QUEUED'):break
                        assert time.monotonic()<deadline,task;time.sleep(.02)
                    assert not errors,errors
                    assert task['status']==expected,task
                    if protocol=='native':
                        history=api('/v1/chat/history?session=image-test')
                        assert any(event.get('commentary')=='正在读取图片以核实交付内容。' for run in history['runs'] for event in run.get('trace',[])),history
                    if expected=='DONE':
                        previews=[s['image_preview'] for s in task['steps'] if s.get('image_preview')]
                        assert previews and len({p['id'] for p in previews})==1,task
                        preview=previews[-1];result=api('/v1/uploads/preview?id='+preview['id']+'&session=image-test')
                        assert base64.b64decode(result['data_url'].split(',',1)[1])==data
                        assert base64.b64encode(data).decode() not in json.dumps(task),'raw image leaked into progress/context text'
                        try:api('/v1/uploads/preview?id='+preview['id']+'&session=other');raise AssertionError('cross-session preview exposed')
                        except urllib.error.HTTPError as e:assert e.code==404
                    else:assert not any(s.get('image_preview') for s in task['steps'])
                    print('PASS #241:',protocol,mode,task['status'],'calls',len(calls),flush=True)
                finally:proc.terminate();proc.wait(timeout=5)
    finally:server.shutdown();server.server_close()
if __name__=='__main__':
    for protocol in ('legacy','native','anthropic'):
        for mode in ('existing','jpeg','cached','generated','invalid'):exercise(protocol,mode)
