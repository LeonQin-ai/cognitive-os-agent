"""Benchmark smoke: verify measured work, recovery and rollback, without timing thresholds."""
import json, os, subprocess, tempfile
from pathlib import Path
backend=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coa-bench-smoke-') as state:
    exe=Path(os.environ.get('COA_BENCH_BINARY',str(backend/'build'/('bench-runtime.exe' if os.name=='nt' else 'bench-runtime'))))
    p=subprocess.run([str(exe),state,'1000','4','8','1000','8'],cwd=backend,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
    assert p.returncode==0,(p.returncode,p.stdout,p.stderr)
    start=p.stdout.find('\n{\n'); report=json.loads(p.stdout[start+1:].strip())
    assert report['agents']==1000 and report['tasks_completed']==1000 and report['workers']==8,report
    assert report['aggregate_context_bytes']==4*1024*1024 and report['dag_depth']==8,report
    assert report['tasks_submitted']==report['tasks_admitted']==report['tasks_completed'] and report['tasks_failed']==report['tasks_rejected']==report['queue_at_end']==report['active_at_end']==0,report
    assert report['submission_samples'] and report['context_probes_per_task']==64,report
    assert report['rollback_verified'] is True and report['retained_tasks']<=64,report
    assert report['elapsed_ms']>0 and report['tasks_per_second']>0 and report['peak_rss_bytes']>0,report
    assert report['scheduler_latency_p50_ms']<=report['scheduler_latency_p95_ms']<=report['scheduler_latency_max_ms'],report
    print('PASS: 1000 registered agents / 1000 completed runtime tasks / touched context / DAG / failure recovery / verified rollback')

with tempfile.TemporaryDirectory(prefix='coa-context-smoke-') as state:
    p=subprocess.run([str(exe),'--context',state,'2','1','1'],cwd=backend,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
    assert p.returncode==0,(p.returncode,p.stdout,p.stderr)
    r=json.loads(p.stdout[p.stdout.find('{'):])
    assert r['source_bytes']==4*1024*1024 and r['canonical_checksums_verified'],r
    assert r['initial']['hot']==3 and r['evicted']['cold']==3 and r['evicted']['evictions']==3,r
    assert r['evicted']['materialized_bytes']==0 and r['overview']['warm']==3 and r['overview']['materialized_bytes']<16384,r
    assert r['recovered']['hot']==3 and r['recovered']['materialized_bytes']>=r['source_bytes'],r
    print('PASS: actual MMU HOT allocation -> eviction -> overview reduction -> HOT rehydration -> canonical disk checksum recovery')

with tempfile.TemporaryDirectory(prefix='coa-backlog-smoke-') as state:
    p=subprocess.run([str(exe),state,'1000','4','8','1000','8','backlog'],cwd=backend,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
    assert p.returncode==0,(p.returncode,p.stdout,p.stderr)
    r=json.loads(p.stdout[p.stdout.find('{'):])
    assert r['backlog_gate'] and r['observed_queue_max']>=992 and r['tasks_completed']==1000,r
    assert r['active_at_end']==r['queue_at_end']==r['tasks_rejected']==0,r
    print('PASS: held workers create a real backlog; admitted tasks drain with conservation')
