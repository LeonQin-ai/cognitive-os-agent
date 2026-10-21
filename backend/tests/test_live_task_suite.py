"""Opt-in paid API regressions through Cognitive OS Agent, never direct LLM calls.

--config points to installed configuration; keys are never printed. All files
and task evidence stay in build/. The harness creates only OCR/header/search test inputs and
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
assert all(c in ('source','ppt','image','ocr','diagram_ocr','analysis','html','header_recovery','search_discovery','capabilities','quoted_read','translation','howto','missing_recovery','ipc_analysis','creative_write','unrelated_write_failure','git_diagnostic','external_recovery','rename_story','shell_artifact') for c in selected)
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
            '--state',str(state),'--workspace',str(output),'serve',str(port)],cwd=backend,stdout=log,stderr=log,env={**os.environ,'COA_TOOL_PROTOCOL':'native'},
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
                    if case=='capabilities':
                        prompt='你有什么特殊功能'
                    elif case=='quoted_read':
                        prompt='解释“完整阅读全部源代码”这句话的含义。这里只讨论概念，不要调用工具或读取文件。'
                    elif case=='translation':
                        prompt='把这句话翻译成英语：创建文件，然后删除文件。这是翻译任务，不要执行这些操作。'
                    elif case=='howto':
                        prompt='请解释如何查看Windows机器的CPU和内存状态，只介绍方法，不执行命令。'
                    elif case=='git_diagnostic':
                        git_dir=state/'git-nonrepo';git_dir.mkdir(exist_ok=True)
                        prompt=f'回归检查：仅实际调用 git 工具一次，args 为 status --short，dir 明确传 {git_dir.as_posix()}。这是无 .git 的临时目录，预期工具失败；不要 git init，不修复目录，不重试、不运行 shell。最后报告被阻塞和实际目录，不能把仓库检查标记为成功。'
                    elif case=='unrelated_write_failure':
                        blocked=active/'blocked-target';blocked.mkdir(exist_ok=True)
                        marker='WRITE_B_'+secrets.token_hex(8)
                        prompt=prefix+f'写入故障回归：先实际调用file_write向 {blocked.as_posix()} 写入A（此路径是已有目录，禁止删除或替换目录）。随后file_write向 {(active/"other.txt").as_posix()} 写入 {marker} 并file_read核对。只执行这两次写入尝试和B读取，不执行shell、不重试或修复A。最终整体必须报告未完成，因为A未写入成功；B成功不能代表A成功。'
                    elif case=='shell_artifact':
                        marker='ARTIFACT-'+secrets.token_hex(5);target=active/'result.json'
                        prompt=f'用Python生成 {target.as_posix()}，JSON内容为 marker={marker}、values=[1,4,9]、verified=true。通过shell工具执行Python命令，outputs参数声明这个产物路径；实际file_read核对内容后给出结果。只生成这一个文件，不操作其他目录或版本控制。'
                    elif case=='rename_story':
                        marker='RENAME-'+secrets.token_hex(5)
                        chapters=[]
                        for i,name in enumerate(('霍去病','黄巢','嬴政'),1):
                            chapter=active/f'chapter{i:02d}.md';chapter.write_text(f'# 第{i}章\n{name}是这段架空历史故事的主人公。他的经历借鉴历史人物事迹，但世界和人物都应为虚构。\n'+marker+f'-{i}\n',encoding='utf-8');chapters.append(chapter)
                        prompt=f'修改 {active.as_posix()} 中这三章小说：主角不要直接叫霍去病、黄巢、嬴政，请改为相互区分的架空姓名，保留事迹意象与架空历史设定，保留每章末尾标记，不增加章节。先读取，再实际修改并核验，最后给出处理结果。只处理这三个章节文件，不操作其他目录或版本控制。'
                    elif case=='creative_write':
                        prompt=prefix+'写一篇600到900字的玄幻架空历史热血短篇，融合霍去病、黄巢、秦始皇和新中国成立的意象，内容完整且有结局。保存为 story.md，实际读取核验正文和结尾后交付。本次是一篇独立短篇，不拓展为连载。'
                    elif case=='missing_recovery':
                        marker='RECOVER-'+secrets.token_hex(5)
                        fixture=active/'recover.txt'
                        prompt=f'文件容错回归：先仅调用file_read读取 {fixture.as_posix()} 一次，让工具报告尚不存在；不要提前创建。观察失败后再用file_write创建同一文件，内容为 {marker}，然后实际file_read同一文件核对。完成后直接返回标记，不重复最初错误步骤。'
                    elif case=='ipc_analysis':
                        opcode=secrets.randbelow(50000)+1000
                        (active/'ipc.h').write_text(f'enum {{ IPC_REQUEST={opcode}, IPC_RESPONSE={opcode+1} }};\nstruct Message {{ int opcode; int value; }};',encoding='utf-8')
                        (active/'server.c').write_text('#include "ipc.h"\nint handle(struct Message m){ if(m.opcode!=IPC_REQUEST)return -1; return m.value+1; }',encoding='utf-8')
                        (active/'client.c').write_text('#include "ipc.h"\nint request(int fd,int value){ struct Message m={IPC_REQUEST,value}; return send_message(fd,&m); }',encoding='utf-8')
                        prompt=f'分析 {active.as_posix()} 示例代码中的服务端和客户端，补充阅读IPC接口定义，说明请求响应流程、错误处理和局限。给出IPC_REQUEST实际数值并引用实际函数。只返回完整技术分析，不创建文件；不要停在还需要阅读的进度说明。'
                    elif case=='source':
                        prompt=prefix+f'阅读这些源码的相关实现并生成 source-overview.md，不要求全文：{json.dumps(source_files,ensure_ascii=False)}。技术开发者概要不少于1500字，覆盖 MMU、上下文组装、工具执行、循环停止和失败恢复，引用实际函数/路径，说明边界与取舍。实际保存文件，并自行检查内容和字数。最终答复给出文件位置。'
                    elif case=='search_discovery':
                        fixture=active/'long-line.txt';fixture.write_text('DISCOVERY_NEEDLE'+'x'*150000,encoding='utf-8')
                        prompt=f'只用一次grep在 {fixture.as_posix()} 查找 DISCOVERY_NEEDLE；output_mode 必须是 files_with_matches，max_scan_bytes 必须是32。只需文件名发现，命中后直接交付文件名，不读文件、不扩大预算、不写文件。'
                    elif case=='external_recovery':
                        project=state/'outside-project';actual=project/'src/addons/rmrs/context.h';actual.parent.mkdir(parents=True)
                        marker='OUTSIDE-'+secrets.token_hex(5);actual.write_text(marker,encoding='utf-8')
                        wrong=(project/'addons/rmrs/context.h').as_posix()
                        prompt=f'项目位于 {project.as_posix()}，不是 Agent 工作区。回归步骤：先仅 file_read 读取 {wrong} 一次，预期不存在；再 glob，path 明确用 {project.as_posix()}、pattern 用 **/context.h；最后 file_read 读取 glob 返回的真实路径，返回文件中的标记。禁止重试错误路径、写文件或运行 shell；成功核对替代路径后完成任务。'
                    elif case=='header_recovery':
                        header=active/'include/context/probe.h';header.parent.mkdir(parents=True)
                        marker='HEADER-'+secrets.token_hex(5)
                        header.write_text('#define RECOVERY_MARKER "'+marker+'"\n',encoding='utf-8')
                        wrong=(active/'src/context/probe.h').as_posix()
                        prompt=f'一次性容错测试：仅在第一步调用 file_read 读取 {wrong} 一次，让工具实际报告错误。此错误是测试预期，之后禁止重试此错误路径。接着查找真实 probe.h 并实际读取，最终返回 RECOVERY_MARKER 的实际值。纠正后直接交付，不重复初始错误步骤，不写文件。'
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
                    if case=='git_diagnostic':
                        assert task.get('status')=='FAILED',task.get('status')
                        steps=task.get('steps',[])
                        assert len(steps)==1 and steps[0]['tool']=='git' and not steps[0]['ok'],steps
                        invocation=json.loads(steps[0]['args'])
                        assert invocation['dir']==git_dir.as_posix(),invocation
                        assert git_dir.as_posix() in steps[0]['out'] and 'not inside a Git repository' in steps[0]['out'],steps
                        assert not (git_dir/'.git').exists()
                        result['checks']={'actual_git_failure':True,'explicit_directory_preserved':True,'no_retry':True}
                        result['passed']=True
                        report['cases'][case]=result;report['attempts'].append(result);save_report()
                        print(json.dumps(result,ensure_ascii=False),flush=True)
                        continue
                    if case=='unrelated_write_failure':
                        assert task.get('status')=='FAILED' and blocked.is_dir(),task.get('status')
                        assert (active/'other.txt').read_text(encoding='utf-8')==marker
                        steps=task.get('steps',[])
                        assert any(st['tool']=='file_write' and not st['ok'] for st in steps)
                        assert any(st['tool']=='file_write' and st['ok'] for st in steps)
                        assert any(st['tool']=='file_read' and st['ok'] for st in steps)
                        result['checks']={'expected_failure_preserved':True,'unrelated_file_verified':True}
                        result['passed']=True
                        report['cases'][case]=result;report['attempts'].append(result);save_report()
                        print(json.dumps(result,ensure_ascii=False),flush=True)
                        continue
                    assert task.get('status')=='DONE','Agent did not finish: '+str(task.get('output',''))[:500]
                    if case in ('capabilities','quoted_read','translation','howto'):
                        text=task.get('output') or ''
                        assert len(text)>10 and not task.get('model_failures'),text
                        assert task.get('tool_calls')==0 and not task.get('steps'),'informational request executed a tool'
                        if case=='translation':assert 'creat' in text.lower() and 'delet' in text.lower(),text
                        result['checks']={'answer_characters':len(text),'completed':True,'no_tools':True}
                    elif case=='shell_artifact':
                        data=json.loads(target.read_text(encoding='utf-8'))
                        assert data=={'marker':marker,'values':[1,4,9],'verified':True},data
                        steps=task.get('steps',[])
                        assert any(st['ok'] and st['tool']=='shell' and target.as_posix() in json.loads(st['args']).get('outputs',[]) for st in steps),steps
                        assert any(st['ok'] and st['tool']=='file_read' and target.as_posix()==json.loads(st['args']).get('path') for st in steps),steps
                        result['checks']={'python_generated':True,'declared_output':True,'read_back_matched':True}
                    elif case=='rename_story':
                        steps=task.get('steps',[])
                        for i,chapter in enumerate(chapters,1):
                            text=chapter.read_text(encoding='utf-8')
                            assert marker+f'-{i}' in text and not any(name in text for name in ('霍去病','黄巢','嬴政')),str(chapter)
                            assert any(st['ok'] and st['tool']=='file_read' and chapter.name in st['args'] for st in steps),steps
                        assert any(st['ok'] and st['tool'] in ('file_edit','file_write','shell') for st in steps),steps
                        result['checks']={'chapters_renamed':3,'markers_retained':True,'read_and_mutated':True}
                    elif case=='creative_write':
                        path=active/'story.md';text=path.read_text(encoding='utf-8')
                        assert 500<=len(text)<=4000, 'Unexpected story size'
                        steps=task.get('steps',[])
                        assert any(st['tool']=='file_write' and st['ok'] for st in steps)
                        assert any(st['tool']=='file_read' and st['ok'] and 'story.md' in st['args'] for st in steps)
                        result['checks']={'characters':len(text),'written_and_read':True}
                        result['artifacts']['story']=str(path)
                    elif case=='missing_recovery':
                        reads=[st for st in task.get('steps',[]) if st['tool']=='file_read']
                        assert reads and not reads[0]['ok'] and any(st['ok'] for st in reads[1:]),reads
                        assert fixture.read_text(encoding='utf-8')==marker and marker in (task.get('output') or '')
                        result['checks']={'actual_read_failure':True,'actual_repaired_read':True,'marker_matched':True}
                    elif case=='ipc_analysis':
                        text=task.get('output') or ''
                        reads=[st for st in task.get('steps',[]) if st['tool']=='file_read' and st['ok']]
                        assert all(any(name in st['args'] for st in reads) for name in ('client.c','server.c','ipc.h')),reads
                        assert str(opcode) in text and 'handle' in text and 'request' in text and len(text)>200,text
                        result['checks']={'client_server_and_ipc_read':True,'hidden_opcode_matched':True,'answer_characters':len(text)}
                    elif case=='source':
                        path=active/'source-overview.md';text=path.read_text(encoding='utf-8');result['artifacts']['overview']=str(path)
                        result['checks']={'characters':len(text),'source_references':sum(p in text for p in ('mmu.h','context_builder.c','reasoning.c'))}
                        assert len(text)>=1500 and result['checks']['source_references']>=2
                    elif case=='search_discovery':
                        steps=task.get('steps',[])
                        assert steps and task.get('tool_calls')==1 and all(st['tool']=='grep' and st['ok'] for st in steps),steps
                        assert all(st['out']=='previous search result reused' for st in steps[1:]),steps
                        call=json.loads(steps[0]['args'])
                        assert call['output_mode']=='files_with_matches' and call['max_scan_bytes']==32
                        assert 'long-line.txt' in steps[0]['out'] and 'byte budget' not in steps[0]['out']
                        assert 'long-line.txt' in (task.get('output') or '')
                        result['checks']={'first_match_within_32_bytes':True,'single_actual_grep':True,'no_full_read':True,'cached_repeat_requests':len(steps)-1}
                    elif case=='external_recovery':
                        steps=task.get('steps',[]);reads=[st for st in steps if st['tool']=='file_read']
                        assert reads and not reads[0]['ok'] and json.loads(reads[0]['args'])['path']==wrong,reads
                        assert any(st['ok'] and json.loads(st['args'])['path']==actual.as_posix() for st in reads),reads
                        assert any(st['tool']=='glob' and st['ok'] and json.loads(st['args'])['path']==project.as_posix() for st in steps),steps
                        assert marker in (task.get('output') or '')
                        result['checks']={'external_search_root':True,'wrong_path_failed':True,'discovered_replacement_read':True,'marker_matched':True}
                    elif case=='header_recovery':
                        reads=[st for st in task.get('steps',[]) if st.get('tool')=='file_read']
                        assert any(not st.get('ok') and 'src/context/probe.h' in st.get('args','').lower().replace('\\','/') for st in reads)
                        assert any(st.get('ok') and 'include/context/probe.h' in st.get('args','').lower().replace('\\','/') for st in reads)
                        assert marker in (task.get('output') or '')
                        result['checks']={'missing_src_observed':True,'real_include_read':True,'hidden_marker_matched':True}
                    elif case=='analysis':
                        text=task.get('output') or ''
                        assert len(text)>1500 and all(k in text for k in ('mmu','context_builder','embedding'))
                        result['checks']={'answer_characters':len(text),'core_modules':True}
                    elif case=='html':
                        path=active/'memory-report.html';text=path.read_text(encoding='utf-8')
                        from html.parser import HTMLParser
                        class Diagrams(HTMLParser):
                            flows=0;nodes=0
                            def handle_starttag(self,tag,attrs):
                                classes=dict(attrs).get('class','').split()
                                self.flows+=int('flow' in classes)
                                self.nodes+=int('node' in classes)
                        diagrams=Diagrams();diagrams.feed(text)
                        svg='<svg' in text.lower()
                        assert '<html' in text.lower() and len(text)>2000
                        assert svg or (diagrams.flows>=2 and diagrams.nodes>=4),'Missing SVG or CSS architecture/flow diagrams'
                        result['checks']={'html_characters':len(text),'svg':svg,'css_flows':diagrams.flows,'css_nodes':diagrams.nodes}
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
