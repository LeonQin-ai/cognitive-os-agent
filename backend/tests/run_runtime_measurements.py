"""Reproducible runtime scale + real MMU pressure measurements, no paid LLM.
Use --full explicitly for 100k tasks and 650MiB/1GiB canonical contexts.
Use --million for 100/1000 roles x 1M tasks, three runs each, without MMU reruns.
Temporary runtime state is removed by TemporaryDirectory; only final JSON survives.
"""
import argparse,json,os,platform,subprocess,tempfile
from pathlib import Path
from datetime import datetime,timezone,timedelta
parser=argparse.ArgumentParser()
mode=parser.add_mutually_exclusive_group()
mode.add_argument('--full',action='store_true')
mode.add_argument('--million',action='store_true',help='100/1000 roles x 1,000,000 tasks, 3 repetitions; scale only')
parser.add_argument('--compiler',default='unspecified; supply actual compiler/version/flags with --compiler')
parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'build/runtime-measurements')
parser.add_argument('--date',default=datetime.now(timezone(timedelta(hours=8))).date().isoformat())
args=parser.parse_args(); backend=Path(__file__).resolve().parents[1]
exe=Path(os.environ.get('COA_BENCH_BINARY',str(backend/'build'/('bench-runtime.exe' if os.name=='nt' else 'bench-runtime'))))
meta={'date':args.date,'platform':platform.platform(),'cpu':platform.processor(),'compiler':args.compiler,'repetitions':3 if args.full or args.million else 1}
args.output.mkdir(parents=True,exist_ok=True)
def run(options):
    p=subprocess.run([str(exe),*map(str,options)],cwd=backend,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=240)
    if p.returncode: raise RuntimeError((p.returncode,p.stdout[-2000:],p.stderr[-1000:]))
    return json.loads(p.stdout[p.stdout.find('{'):])
scale=[]
for agents,tasks in ([(100,1000000),(1000,1000000)] if args.million else [(100,10000),(1000,10000),(100,100000),(1000,100000)] if args.full else [(100,1000),(1000,1000)]):
    for repeat in range(meta['repetitions']):
        with tempfile.TemporaryDirectory(prefix='coa-scale-final-') as state:
            r=run([state,tasks,8,32,agents,8]); r['repeat']=repeat+1;scale.append(r)
            assert r['tasks_completed']==r['tasks_submitted']==r['tasks_admitted']==tasks
            assert r['tasks_failed']==r['tasks_rejected']==r['active_at_end']==r['queue_at_end']==0
            assert r['retained_tasks']<=64 and r['rollback_verified']
            print(f"scale agents={agents} tasks={tasks} repeat={repeat+1} elapsed_ms={r['elapsed_ms']:.3f}",flush=True)
(args.output/f'runtime-{"million" if args.million else "scale"}-{args.date}.json').write_text(json.dumps(dict(meta,scope='actual scheduler + agent pool; 64 context probes per task; no remote inference',runs=scale),ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
if args.million: raise SystemExit(0)
pressure=[]
for sizes in ([(512,128,10),(768,128,128)] if args.full else [(2,1,1)]):
    with tempfile.TemporaryDirectory(prefix='coa-mmu-final-') as state:
        r=run(['--context',state,*sizes]); r['context_mib']=sizes;pressure.append(r)
        assert r['canonical_checksums_verified'] and r['initial']['hot']==r['recovered']['hot']==3
        assert r['evicted']['cold']==3 and r['evicted']['materialized_bytes']==0
        print(f"pressure MiB={sizes} peak_rss_MiB={r['peak_rss_bytes']/1048576:.2f} eviction_ms={r['budget_shrink_recall_ms']:.3f}",flush=True)
(args.output/f'runtime-context-pressure-{args.date}.json').write_text(json.dumps(dict(meta,repetitions=1,runs=pressure),ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
