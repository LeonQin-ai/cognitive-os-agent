"""Retained runtime evidence reconciles with English/Chinese published metrics."""
import json,re,statistics
from pathlib import Path
from xml.etree import ElementTree

root=Path(__file__).resolve().parents[2]
documents=[root/'README.md',root/'README.zh-CN.md',root/'backend/docs/runtime-benchmark-expanded.md']
texts=[p.read_text(encoding='utf-8') for p in documents]
for filename in ['runtime-scale-2026-10-14.json','runtime-million-2026-10-14.json']:
    data=json.loads((root/'backend/docs'/filename).read_text(encoding='utf-8'))
    groups={}
    for run in data['runs']:
        count=run['tasks_submitted']
        assert run['tasks_completed']==run['tasks_admitted']==count
        assert not any(run[k] for k in ['tasks_failed','tasks_rejected','queue_at_end','active_at_end'])
        assert run['retained_tasks']<=64 and run['rollback_verified']
        groups.setdefault((run['agents'],count),[]).append(run)
    for (agents,count),runs in groups.items():
        assert len(runs)==data['repetitions']==3
        median=lambda key: statistics.median(r[key] for r in runs)
        row=(f'| {agents:,} | {count:,} | {median("elapsed_ms"):.3f} | '
             f'{median("tasks_per_second"):,.0f} | {median("scheduler_latency_p50_ms"):.3f} / '
             f'{median("scheduler_latency_p95_ms"):.3f} / {median("scheduler_latency_p99_ms"):.3f} | '
             f'{max(r["peak_rss_bytes"] for r in runs)/1048576:.2f} |')
        assert all(row in text for text in texts),row
    if 'million' in filename:
        assert set(groups)=={(100,1000000),(1000,1000000)}
        assert sum(r['tasks_completed'] for r in data['runs'])==6000000
for doc,text in zip(documents,texts):
    for target in re.findall(r'\]\(([^)]+)\)',text):
        if '://' not in target and not target.startswith('#'):
            assert (doc.parent/target.split('#')[0]).exists(),(doc,target)
for filename in ['runtime-scale-2026-10-14.svg','runtime-memory-2026-10-14.svg']:
    ElementTree.parse(root/'backend/docs'/filename)
print('PASS: retained scale/million counts reconcile, three-run medians match both READMEs and report, local links and SVGs valid')
