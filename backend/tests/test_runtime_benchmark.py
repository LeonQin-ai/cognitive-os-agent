"""Benchmark smoke: verify measured work, recovery and rollback, without timing thresholds."""
import json, os, subprocess, tempfile
from pathlib import Path
backend=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coa-bench-smoke-') as state:
    exe=backend/'build'/('bench-runtime.exe' if os.name=='nt' else 'bench-runtime')
    p=subprocess.run([str(exe),state,'1000','4','8'],cwd=backend,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
    assert p.returncode==0,(p.returncode,p.stdout,p.stderr)
    start=p.stdout.find('\n{\n'); report=json.loads(p.stdout[start+1:].strip())
    assert report['agents']==100 and report['tasks_completed']==1000 and report['workers']==8,report
    assert report['aggregate_context_bytes']==4*1024*1024 and report['dag_depth']==8,report
    assert report['rollback_verified'] is True and report['retained_tasks']<=64,report
    assert report['elapsed_ms']>0 and report['tasks_per_second']>0 and report['peak_rss_bytes']>0,report
    assert report['scheduler_latency_p50_ms']<=report['scheduler_latency_p95_ms']<=report['scheduler_latency_max_ms'],report
    print('PASS: 100 registered agents / 1000 completed runtime tasks / touched context / DAG / failure recovery / verified rollback')
