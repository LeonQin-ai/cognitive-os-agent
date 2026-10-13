"""Export figures from retained measurements (requires matplotlib)."""
import json,statistics
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
root=Path(__file__).resolve().parents[1]/'docs'
runs=json.loads((root/'runtime-scale-2026-10-14.json').read_text())['runs']
groups={}
for r in runs: groups.setdefault((r['agents'],r['tasks_completed']),[]).append(r)
keys=sorted(groups,key=lambda k:(k[1],k[0])); labels=[f'{a} agents\n{n:,} tasks' for a,n in keys]
fig,axes=plt.subplots(1,2,figsize=(11,4.2))
axes[0].bar(range(len(keys)),[statistics.median(r['tasks_per_second'] for r in groups[k]) for k in keys],color=['#5184a8','#217977']*2)
axes[0].set_ylabel('Completed tasks / second'); axes[0].set_title('Scheduler + role lookup (8 workers)')
for metric,label in [('scheduler_latency_p50_ms','P50'),('scheduler_latency_p95_ms','P95'),('scheduler_latency_p99_ms','P99')]:
    axes[1].plot(range(len(keys)),[statistics.median(r[metric] for r in groups[k]) for k in keys],'o-',label=label)
axes[1].set_ylabel('Queue latency (ms), median of 3 runs'); axes[1].set_title('Submit to worker start'); axes[1].legend()
for ax in axes:
    ax.set_xticks(range(len(keys)),labels);ax.spines[['top','right']].set_visible(False);ax.grid(axis='y',alpha=.2)
fig.suptitle('2026-10-14 / deterministic local workload / 64 context probes per task',fontsize=10)
fig.tight_layout();fig.savefig(root/'runtime-scale-2026-10-14.svg');fig.savefig(root.parent/'build/runtime-scale-preview.png',dpi=130);plt.close(fig)
k=(1000,100000); ordered=sorted(groups[k],key=lambda r:r['elapsed_ms']);r=ordered[len(ordered)//2]
fig,axes=plt.subplots(2,1,figsize=(9,5.4),sharex=True)
s=r['submission_samples']; x=[q['elapsed_ms']/1000 for q in s]
axes[0].plot(x,[q['rss_bytes']/1048576 for q in s],label='Current RSS',color='#217977')
axes[0].plot(x,[q['peak_rss_bytes']/1048576 for q in s],label='OS peak RSS',color='#b87b39',linestyle='--')
axes[0].set_ylabel('MiB');axes[0].legend();axes[0].set_title('1000 registered roles / 100,000 tasks / representative median-duration run')
axes[1].plot(x,[q['queued'] for q in s],label='Sampled queued tasks',color='#5184a8');axes[1].set_ylabel('Queued tasks');axes[1].set_xlabel('Seconds since first submission')
for ax in axes: ax.spines[['top','right']].set_visible(False);ax.grid(alpha=.2)
fig.tight_layout();fig.savefig(root/'runtime-memory-2026-10-14.svg');fig.savefig(root.parent/'build/runtime-memory-preview.png',dpi=130);plt.close(fig)
