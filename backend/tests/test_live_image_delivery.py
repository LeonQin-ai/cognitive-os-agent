"""One bounded configured API task; the Agent authors and executes the image generator."""
import argparse,base64,json,os,socket,subprocess,tempfile,time,urllib.request
from pathlib import Path
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(__doc__);p.add_argument('--config',type=Path,required=True);p.add_argument('--report',type=Path,default=root/'build/live-image-delivery.json');a=p.parse_args()
cfg={k:v for k,v in json.loads(a.config.read_text(encoding='utf-8-sig')).items() if k.startswith('llm.')}
cfg.update({'http.bind':'127.0.0.1','reasoning.max_rounds':12,'scheduler.workers':1})
report={'passed':False,'model':cfg.get('llm.model'),'scope':'generated PNG + actual file_read + vision request + session-scoped preview'}
with tempfile.TemporaryDirectory(prefix='coa-live-image-') as folder:
 state=Path(folder);(state/'cognitive-os-agent.json').write_text(json.dumps(cfg),encoding='utf8')
 with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
 def api(method,path,body=None):
  request=urllib.request.Request(f'http://127.0.0.1:{port}'+path,method=method,data=json.dumps(body).encode() if body is not None else None,headers={'Content-Type':'application/json'})
  with urllib.request.urlopen(request,timeout=10) as r:return json.load(r)
 a.report.parent.mkdir(parents=True,exist_ok=True)
 with a.report.with_suffix('.log').open('w',encoding='utf8') as log:
  proc=subprocess.Popen([str(root/'build'/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),'--state',folder,'--workspace',folder,'serve',str(port)],cwd=root,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0));tid=None
  try:
   for _ in range(150):
    try:api('GET','/v1/scheduler');break
    except OSError:assert proc.poll() is None;time.sleep(.1)
   prompt=('实际生成 image.png：240×120 RGB 图片，左半纯红(255,0,0)，右半纯蓝(0,0,255)。'
           '用 file_write 编写 render.py，仅用 Python 标准库 struct/zlib 生成 PNG，实际 shell 执行 python render.py，'
           'shell 必须声明 outputs:["image.png"]。随后 file_read image.png，依据实际图像交付。'
           '不要安装软件或访问网络，不要只写脚本不执行，不要把 base64 放进最终答复。最终答复包含 image.png 及左右颜色。'
           '如使用 declare_task，必须单独调用并等待回执，再做工具操作。')
   start=time.monotonic();tid=api('POST','/v1/chat',{'message':prompt,'session':'live-image','acceptance':{'artifacts':['image.png'],'requires_action':True}})['id']
   while True:
    task=api('GET',f'/v1/tasks/{tid}')
    if task['status'] not in ('RUNNING','QUEUED'):break
    if time.monotonic()-start>180 or task.get('llm_calls',0)>14 or task.get('model_failures',0)>2:
     api('DELETE',f'/v1/tasks/{tid}');raise RuntimeError('bounded image task stopped; no automatic rerun')
    time.sleep(.2)
   report['seconds']=round(time.monotonic()-start,3);report['task']=task
   assert task['status']=='DONE',task.get('output')
   previews=[s['image_preview'] for s in task.get('steps',[]) if s.get('image_preview')]
   assert previews,'no verified image read receipt'
   preview=api('GET','/v1/uploads/preview?id='+previews[-1]['id']+'&session=live-image')
   content=base64.b64decode(preview['data_url'].split(',',1)[1]);assert content==(state/'image.png').read_bytes()
   a.report.with_suffix('.png').write_bytes(content)
   from PIL import Image
   import io
   image=Image.open(io.BytesIO(content)).convert('RGB')
   assert image.size==(240,120) and image.getpixel((30,60))==(255,0,0) and image.getpixel((210,60))==(0,0,255)
   assert 'image.png' in task.get('output','')
   report['preview_bytes']=len(content);report['pixel_checks_passed']=True;report['passed']=True
  except Exception as e:report['error']=str(e)
  finally:
   try:
    if tid is not None:report['task']=api('GET',f'/v1/tasks/{tid}')
    report['usage']=api('GET','/v1/usage')
   except OSError:pass
   proc.terminate();proc.wait(timeout=10)
 a.report.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
print(json.dumps({k:v for k,v in report.items() if k!='task'},ensure_ascii=False))
raise SystemExit(0 if report['passed'] else 1)
