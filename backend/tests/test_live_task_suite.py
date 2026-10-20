"""Opt-in paid API regressions through Cognitive OS Agent, never direct LLM calls.

--config points to installed configuration; keys are never printed. All files
and task evidence stay in build/. The harness creates only OCR test inputs and
read-only verification reports; the agent must produce every requested artifact.
"""
import argparse
import json
import os
import secrets
import socket
import sys
import subprocess
import tempfile
import time
import urllib.request
import zipfile
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

if hasattr(sys.stdout,"reconfigure"): sys.stdout.reconfigure(encoding="utf-8")

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--config',type=Path,required=True)
parser.add_argument('--cases',default='source,ppt,image,ocr,diagram_ocr')
parser.add_argument('--output',type=Path)
parser.add_argument('--timeout',type=int,default=900,help='Per-test wall deadline; no production token/round cap')
args=parser.parse_args()
backend=Path(__file__).resolve().parents[1]
output=args.output.resolve() if args.output else backend/'build'/time.strftime('live-api-suite-%Y%m%d-%H%M%S')
output.mkdir(parents=True,exist_ok=True)
original=json.loads(args.config.read_text(encoding='utf-8'))
config={k:v for k,v in original.items() if k.startswith('llm.')}
if config.get('llm.provider') in (None,'mock') or not config.get('llm.api_key'):
    raise SystemExit('A configured real API is required')
config.update({'reasoning.max_rounds':-1,'llm.routing':'fixed','scheduler.workers':1})
report_path=output/'report.json'
report=json.loads(report_path.read_text(encoding='utf-8')) if report_path.exists() else {'model':config.get('llm.model'),'cases':{},'attempts':[]}
selected=args.cases.split(',')
assert all(c in ('source','ppt','image','ocr','diagram_ocr','analysis','html') for c in selected)
source_files=[(backend/p).as_posix() for p in ('include/context/mmu.h','src/retrieval/context_builder.c','src/cognition/reasoning.c')]
active=None
last_task={}

def save_report():
    report_path.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')

def task_summary(task):
    return {k:task.get(k) for k in ('status','round','llm_calls','tool_calls','model_failures','tokens_in','tokens_out','usage_available','elapsed_ms')}

with tempfile.TemporaryDirectory(prefix='coa-live-suite-state-') as folder:
    state=Path(folder)
    (state/'cognitive-os-agent.json').write_text(json.dumps(config),encoding='utf-8')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
    def request(path,body=None):
        data=body if isinstance(body,bytes) else json.dumps(body).encode() if body is not None else None
        req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=data,headers={'Content-Type':'application/octet-stream' if isinstance(body,bytes) else 'application/json'})
        with urllib.request.urlopen(req,timeout=15) as response: raw=response.read()
        try:return json.loads(raw)
        except (UnicodeDecodeError,json.JSONDecodeError):
            if active:
                (active/'invalid-api-response.bin').write_bytes(raw)
                (active/'last-valid-task.json').write_text(json.dumps(last_task,ensure_ascii=False),encoding='utf-8')
            raise
    def run_task(prompt,session,attachments=None):
        global last_task
        body={'message':prompt,'session':session}
        if attachments:body['attachments']=attachments
        task_id=request('/v1/chat',body)['id']
        started=time.monotonic();printed=-30
        while time.monotonic()-started<args.timeout:
            last_task=request('/v1/tasks/'+str(task_id))
            if time.monotonic()-started-printed>=30:
                print(json.dumps({'case':active.name,**task_summary(last_task)},ensure_ascii=False),flush=True)
                printed=time.monotonic()-started
            if last_task.get('status') not in ('RUNNING','QUEUED','CANCELLING'):
                (active/'task.json').write_text(json.dumps(last_task,ensure_ascii=False,indent=2),encoding='utf-8')
                return last_task
            time.sleep(.5)
        (active/'last-valid-task.json').write_text(json.dumps(last_task,ensure_ascii=False),encoding='utf-8')
        raise TimeoutError('Test deadline exceeded; isolated process must stop before further paid tasks')
    with (output/'server.log').open('w',encoding='utf-8') as log:
        process=subprocess.Popen([str(backend/'build'/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')),
            '--state',str(state),'--workspace',str(output),'serve',str(port)],cwd=backend,stdout=log,stderr=log,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            for _ in range(100):
                try:request('/v1/scheduler');break
                except OSError:
                    if process.poll() is not None:raise RuntimeError('Agent exited during startup')
                    time.sleep(.1)
            (output/'monitor.json').write_text(json.dumps({'port':port,'process_id':process.pid}),encoding='utf-8')
            print(json.dumps({'output':str(output),'port':port,'cases':selected,'model':config.get('llm.model')}),flush=True)
            for case in selected:
                active=output/(case+'-'+time.strftime('%H%M%S'));active.mkdir()
                last_task={};session='regression-'+case+'-'+secrets.token_hex(3)
                result={'case':case,'directory':str(active),'artifacts':{},'checks':{}}
                try:
                    prefix=f'这是实际交付任务，不要只描述计划。项目源码仅供查阅，不要改动项目文件。写入限定在 {active.as_posix()}。已有结果足够后直接交付，不重复未变化的检查；工具失败按真实错误纠正并重新验证。'
                    attached=None
                    if case=='source':
                        prompt=prefix+f'阅读这些源码的相关实现并生成 source-overview.md，不要求全文：{json.dumps(source_files,ensure_ascii=False)}。技术开发者概要不少于1500字，覆盖 MMU、上下文组装、工具执行、循环停止和失败恢复，引用实际函数/路径，说明边界与取舍。实际保存文件，并自行检查内容和字数。最终答复给出文件位置。'
                    elif case=='analysis':
                        prompt=f'分析一下 {backend.as_posix()} 代码中的上下文工程怎么实现的。基于实际源码，给出完整技术分析，覆盖 mmu.c、context_builder.c、attention.c、embedding.c，说明上下文预算、缓存、检索及失败边界。请直接交付分析，不写文件，不只返回接下来要做什么。'
                    elif case=='html':
                        prompt=prefix+f'阅读 {backend.as_posix()}/src/memory/ 下 memory.c、episode.c、vector.c 的相关实现，生成 memory-report.html，包含中文架构说明、架构图和流程图，可以使用内联SVG。实际写入HTML后使用工具读取核对，确认内容完整后最终交付，不要只描述待检查工作。'
                    elif case=='ppt':
                        prior=report['cases'].get('source',{}).get('artifacts',{}).get('overview','')
                        prompt=prefix+f'生成12–16页中文技术深度PPT technical-runtime.pptx。可复用概要 {prior}，必要时检索相关源码 {json.dumps(source_files,ensure_ascii=False)}，不要求全文。覆盖记忆分层、上下文、工具、规划执行闭环、完成证据、恢复、并发和局限。至少10页正文超过80字、至少10页备注超过100字、至少6页标注源码函数/路径。可以用python-pptx，自行编写执行并纠正脚本错误；真实打开PPT核对并写 verification.json。只有文件和核验都完成才交付。'
                    elif case=='image':
                        prompt=prefix+'生成一张1600x900 PNG技术架构图 architecture.png，使用可用绘图能力或Python/Pillow实际生成，不要仅返回代码或Markdown。白色背景、至少5个有内容的模块，用英文标签 Input、Context、Planner、Tools、Verify，包含连接关系和循环箭头，布局清楚，至少使用8种颜色。检查PNG可打开且尺寸正确，再交付文件。'
                    elif case=='ocr':
                        marker='COA-'+''.join(secrets.choice('ABCDEFGHJKLMNPQRSTUVWXYZ23456789') for _ in range(8))
                        expected=['认知运行时','缓存保留已读页','磁盘保存原文',marker,'318.42']
                        image=Image.new('RGB',(1500,900),'white');draw=ImageDraw.Draw(image)
                        font_path=next((p for p in (Path('C:/Windows/Fonts/msyh.ttc'),Path('C:/Windows/Fonts/simhei.ttf')) if p.exists()),None)
                        if not font_path:raise RuntimeError('Chinese OCR fixture font missing')
                        font=ImageFont.truetype(str(font_path),52)
                        for i,line in enumerate(['认知运行时 OCR 回归','编号: '+marker,'总额: 318.42','缓存保留已读页','磁盘保存原文']):draw.text((90,80+i*140),line,font=font,fill='black')
                        fixture=active/'ocr-input.png';image.save(fixture)
                        uploaded=request('/v1/upload?name=ocr-input.png&session='+session,fixture.read_bytes());attached=[uploaded['id']]
                        prompt='请识别附件图片的全部文字，按原行顺序输出。保留编号和金额，不猜测看不清的文字。'
                    else:
                        generated=report['cases'].get('image',{}).get('artifacts',{}).get('png')
                        if not generated:raise RuntimeError('Generated image is unavailable for upload regression')
                        uploaded=request('/v1/upload?name=architecture.png&session='+session,Path(generated).read_bytes());attached=[uploaded['id']]
                        prompt='读取附件架构图，列出图上的全部英文模块名，并简要说明连接关系；看不清时明确说明。'
                    (active/'prompt.txt').write_text(prompt,encoding='utf-8')
                    task=run_task(prompt,session,attached);result.update(task_summary(task))
                    if case=='ocr':(active/'expected.json').write_text(json.dumps(expected,ensure_ascii=False),encoding='utf-8')
                    assert task.get('status')=='DONE','Agent did not finish: '+str(task.get('output',''))[:500]
                    if case=='source':
                        path=active/'source-overview.md';text=path.read_text(encoding='utf-8');result['artifacts']['overview']=str(path)
                        result['checks']={'characters':len(text),'source_references':sum(p in text for p in ('mmu.h','context_builder.c','reasoning.c'))}
                        assert len(text)>=1500 and result['checks']['source_references']>=2
                    elif case=='analysis':
                        text=task.get('output') or ''
                        assert len(text)>1500 and all(k in text for k in ('mmu','context_builder','embedding'))
                        result['checks']={'answer_characters':len(text),'core_modules':True}
                    elif case=='html':
                        path=active/'memory-report.html';text=path.read_text(encoding='utf-8')
                        assert '<html' in text.lower() and '<svg' in text.lower() and len(text)>2000
                        result['checks']={'html_characters':len(text),'svg':True}
                        result['artifacts']['html']=str(path)
                    elif case=='ppt':
                        from pptx import Presentation
                        path=active/'technical-runtime.pptx'
                        with zipfile.ZipFile(path) as z:assert z.testzip() is None
                        deck=Presentation(path);texts=['\n'.join(sh.text for sh in sl.shapes if sh.has_text_frame) for sl in deck.slides]
                        notes=[sl.notes_slide.notes_text_frame.text if sl.has_notes_slide else '' for sl in deck.slides]
                        result['checks']={'slides':len(texts),'body80':sum(len(t)>80 for t in texts),'notes100':sum(len(n)>100 for n in notes),'cited':sum(any(k in t for k in ('src/','include/','mmu','context_builder','reasoning')) for t in texts)}
                        json.loads((active/'verification.json').read_text(encoding='utf-8'))
                        assert 12<=len(texts)<=16 and result['checks']['body80']>=10 and result['checks']['notes100']>=10 and result['checks']['cited']>=6
                        result['artifacts']['pptx']=str(path)
                    elif case=='image':
                        path=active/'architecture.png'
                        with Image.open(path) as im:
                            im.load();size=im.size;colors=im.convert('RGB').getcolors(maxcolors=2000000)
                            result['checks']={'size':size,'format':im.format,'colors':len(colors) if colors else 2000001}
                        assert size==(1600,900) and result['checks']['format']=='PNG' and result['checks']['colors']>=8
                        result['artifacts']['png']=str(path);result['checks']['method']='Agent-created PNG; native model image output not exercised'
                    elif case=='ocr':
                        text=task.get('output') or '';result['checks']={'matched':{e:e in text for e in expected}}
                        assert all(result['checks']['matched'].values()),'OCR did not return all hidden expected strings'
                    else:
                        text=(task.get('output') or '').lower();result['checks']={'labels':{v:v.lower() in text for v in ('Input','Context','Planner','Tools','Verify')}}
                        assert all(result['checks']['labels'].values()),'Uploaded generated diagram labels were not read correctly'
                    result['passed']=True
                except Exception as error:
                    result.update(task_summary(last_task));result['passed']=False;result['error']=str(error)[:1200]
                    if last_task:(active/'last-valid-task.json').write_text(json.dumps(last_task,ensure_ascii=False),encoding='utf-8')
                    report['cases'][case]=result;report['attempts'].append(result);save_report()
                    print(json.dumps(result,ensure_ascii=False),flush=True)
                    if isinstance(error,(TimeoutError,UnicodeDecodeError,json.JSONDecodeError)):raise
                    continue
                report['cases'][case]=result;report['attempts'].append(result);save_report()
                print(json.dumps(result,ensure_ascii=False),flush=True)
            assert all(report['cases'][c].get('passed') for c in selected),'Some real task regressions failed; inspect report.json'
            print('PASS: all selected real Cognitive OS Agent tasks completed and passed independent checks',flush=True)
        finally:
            process.terminate()
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:process.kill();process.wait()
