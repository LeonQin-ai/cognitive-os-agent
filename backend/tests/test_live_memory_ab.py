"""Paid, opt-in same-task A/B for legacy retrieval vs Context MMU.

The harness seeds both stores once through a real Agent tool task, removes the
chat transcript, clones that identical state for every trial, then asks the
same question through a fresh runtime. Secrets stay in temporary config files
and are never copied into the report or console output.
"""
import argparse, json, os, shutil, socket, subprocess, tempfile, time, urllib.request
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--config',type=Path,help='Installed JSON config (keys are never printed)')
parser.add_argument('--anthropic-env',action='store_true',help='Use ANTHROPIC_* environment variables without writing a source config')
parser.add_argument('--trials',type=int,default=3)
parser.add_argument('--timeout',type=int,default=300)
parser.add_argument('--output',type=Path)
args=parser.parse_args()
if args.trials < 0 or args.trials > 10: raise SystemExit('trials must be 0..10')
backend=Path(__file__).resolve().parents[1]
binary=backend/'build'/('cognitive-os-agent.exe' if os.name=='nt' else 'cognitive-os-agent')
if not args.config and not args.anthropic_env: raise SystemExit('--config or --anthropic-env is required')
if args.anthropic_env:
    config={'llm.provider':'anthropic','llm.base_url':os.environ.get('ANTHROPIC_BASE_URL',''),
        'llm.api_key':os.environ.get('ANTHROPIC_AUTH_TOKEN',''),
        'llm.model':os.environ.get('ANTHROPIC_MODEL') or os.environ.get('ANTHROPIC_DEFAULT_SONNET_MODEL','')}
else:
    source=json.loads(args.config.read_text(encoding='utf-8'))
    config={k:v for k,v in source.items() if k.startswith('llm.')}
if config.get('llm.provider') in (None,'mock') or not config.get('llm.api_key'):
    raise SystemExit('A configured real API is required')
config.update({'llm.routing':'fixed','scheduler.workers':1,'reasoning.max_rounds':-1})
output=(args.output or backend/'build'/time.strftime('memory-live-ab-%Y%m%d-%H%M%S')).resolve()
output.mkdir(parents=True,exist_ok=True)
marker='ORCHID-7429'
session='memory-ab-session'

def port():
    with socket.socket() as s: s.bind(('127.0.0.1',0)); return s.getsockname()[1]

run_number=0
def run_server(state,workspace,enabled,task_prompt):
    global run_number
    run_number+=1
    cfg=dict(config); cfg['context.mmu.enabled']=1 if enabled else 0
    (state/'cognitive-os-agent.json').write_text(json.dumps(cfg),encoding='utf-8')
    p=port(); log=(state/'server.log').open('w',encoding='utf-8')
    proc=subprocess.Popen([str(binary),'--state',str(state),'--workspace',str(workspace),'serve',str(p)],
        cwd=backend,stdout=log,stderr=log,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    def request(path,body=None):
        data=json.dumps(body).encode() if body is not None else None
        req=urllib.request.Request(f'http://127.0.0.1:{p}'+path,data=data,headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(req,timeout=15) as response:return json.loads(response.read())
    try:
        for _ in range(150):
            try: request('/v1/scheduler'); break
            except OSError:
                if proc.poll() is not None: raise RuntimeError('runtime exited during startup')
                time.sleep(.1)
        else: raise TimeoutError('runtime startup timed out')
        tid=request('/v1/chat',{'message':task_prompt,'session':session})['id']
        deadline=time.monotonic()+args.timeout
        while time.monotonic()<deadline:
            task=request('/v1/tasks/'+str(tid))
            if task.get('status') not in ('RUNNING','QUEUED','CANCELLING'): return task
            time.sleep(.5)
        raise TimeoutError('task timed out')
    finally:
        proc.terminate()
        try: proc.wait(10)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
        log.close()
        shutil.copy2(state/'server.log',output/f'server-{run_number:02d}-{"mmu" if enabled else "legacy"}.log')

with tempfile.TemporaryDirectory(prefix='coa-memory-ab-') as td:
    root=Path(td); workspace=root/'workspace'; workspace.mkdir()
    (workspace/'seed.txt').write_text(f'Project recovery codename: {marker}\n',encoding='utf-8')
    seed=root/'seed-state'; seed.mkdir()
    proof=workspace/'proof.txt'
    seed_prompt=(f'Read {workspace.as_posix()}/seed.txt with file_read exactly once. '
        f'Write only its project recovery codename to {proof.as_posix()} with file_write, then read proof.txt once to verify it. '
        'Treat the verified codename as a fact worth remembering and reply with the exact codename. '
        'Do not use shell and do not inspect other files.')
    seeded=run_server(seed,workspace,True,seed_prompt)
    if seeded.get('status')!='DONE' or marker not in (seeded.get('output') or ''):
        failure={k:seeded.get(k) for k in ('id','status','output','reason','model_error','tokens_in','tokens_out','llm_calls','tool_calls','model_failures')}
        (output/'seed-failure.json').write_text(json.dumps(failure,ensure_ascii=False,indent=2),encoding='utf-8')
        raise SystemExit(f'seed task failed; inspect {output / "seed-failure.json"}')
    record_dir=seed/'memory'/'records'
    record_files=list(record_dir.glob('*.md')) if record_dir.exists() else []
    if record_files:
        shutil.copytree(record_dir,output/'seed-records',dirs_exist_ok=True)
    # Remove conversational evidence: both arms start from the same durable
    # memory state and the same empty transcript.
    for path in (seed/'chat').glob('*'):
        if path.is_file(): path.unlink()
    rows=[]
    # Interleave arms to reduce time/provider drift while preserving identical state.
    for trial in range(args.trials):
        for arm,enabled in (('legacy',False),('mmu',True)):
            state=root/f'{arm}-{trial+1}'; shutil.copytree(seed,state)
            prompt=('What is the project recovery codename? Reply with only the codename. '
                    'Do not call tools and do not guess.')
            task=run_server(state,workspace,enabled,prompt)
            answer=task.get('output') or ''
            rows.append({'arm':arm,'trial':trial+1,'status':task.get('status'),
                'success':task.get('status')=='DONE' and marker in answer,
                'answer_excerpt':answer[:600],
                'tokens_in':int(task.get('tokens_in') or 0),'tokens_out':int(task.get('tokens_out') or 0),
                'llm_calls':int(task.get('llm_calls') or 0),'tool_calls':int(task.get('tool_calls') or 0),
                'model_failures':int(task.get('model_failures') or 0),
                'elapsed_ms':int(task.get('elapsed_ms') or 0)})
    summary={}
    for arm in ('legacy','mmu'):
        group=[r for r in rows if r['arm']==arm]; n=len(group)
        summary[arm]={'trials':n,'successes':sum(r['success'] for r in group),
            'success_rate':sum(r['success'] for r in group)/n if n else None,
            'tokens_in':sum(r['tokens_in'] for r in group),'tokens_out':sum(r['tokens_out'] for r in group),
            'tokens_total':sum(r['tokens_in']+r['tokens_out'] for r in group),
            'tool_calls':sum(r['tool_calls'] for r in group),
            'model_failures':sum(r['model_failures'] for r in group)}
    report={'model':config.get('llm.model'),'marker':marker,'seed':{
        'tokens_in':int(seeded.get('tokens_in') or 0),'tokens_out':int(seeded.get('tokens_out') or 0),
        'record_count':len(record_files)},
        'rows':rows,'summary':summary}
    (output/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({'output':str(output),'model':report['model'],'summary':summary},ensure_ascii=False))
