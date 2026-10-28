"""Run actual C memory checks in isolated storage; never infer quality from storage success."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

backend = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(__doc__)
parser.add_argument('--binary', type=Path, default=backend/'build'/('cognitive-os-agent-test.exe' if os.name == 'nt' else 'cognitive-os-agent-test'))
parser.add_argument('--report', type=Path, default=backend/'build'/'memory-baseline.json')
args = parser.parse_args()
binary = args.binary.resolve()
started = time.monotonic()
with tempfile.TemporaryDirectory(prefix='coa-memory-baseline-') as state:
    run = subprocess.run([str(binary), '--memory-lifecycle'], cwd=state,
        env={**os.environ, 'COA_TOOL_PROTOCOL': 'legacy'}, capture_output=True,
        encoding='utf-8', errors='replace', timeout=180)
matches = re.findall(r'(\d+) passed, (\d+) failed', run.stdout)
passed, failed = map(int, matches[-1]) if matches else (0, 1)
report = {
    'schema': 1, 'binary': str(binary), 'exit_code': run.returncode,
    'passed_assertions': passed, 'failed_assertions': failed,
    'elapsed_seconds': round(time.monotonic()-started, 3),
    'scope': 'deterministic C memory lifecycle regression; process-local reopen is not crash recovery',
    'paid_model_requests': 0,
    'not_measured': ['semantic Recall@5/Precision@5', '100-agent authorization isolation',
        'abrupt process crash recovery', '500-turn constraint retention',
        'real-model extraction and use', 'memory ablation task benefit',
        'cold/hot retrieval latency percentiles'],
}
args.report.parent.mkdir(parents=True, exist_ok=True)
args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
args.report.with_suffix('.log').write_text(run.stdout+'\n'+run.stderr, encoding='utf-8')
print(json.dumps(report, ensure_ascii=False))
raise SystemExit(0 if run.returncode == 0 and matches and passed and not failed else 1)
