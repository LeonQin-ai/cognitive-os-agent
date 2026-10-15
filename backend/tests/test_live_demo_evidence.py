"""Validate the delivered recording against its captured execution evidence."""
import hashlib
import json
from pathlib import Path
import re

docs=Path(__file__).resolve().parents[1]/'docs'
video=docs/'runtime-demo.mp4'
evidence=json.loads((docs/'runtime-demo-evidence.json').read_text(encoding='utf-8'))
assert 300 <= evidence['duration'] <= 330
assert (evidence['width'],evidence['height']) == (1440,900)
assert video.stat().st_size==evidence['bytes']
assert hashlib.sha256(video.read_bytes()).hexdigest()==evidence['sha256']
assert 'local deterministic fixture' in evidence['model']
tasks=[task for task in evidence['tasks'] if 'status' in task]
assert len(tasks)==6 and len({task['id'] for task in tasks})==6
assert all(task['status']=='DONE' and task['tool_calls']==len(task['steps']) and
           task['steps'] and all(step['ok'] for step in task['steps']) for task in tasks)
assert [step['tool'] for step in tasks[0]['steps']]==['list_dir','file_write','file_read']
pages=[json.loads(step['args']).get('offset_bytes',0) for step in tasks[1]['steps']]
assert len(pages)>1 and pages==list(range(0,len(pages)*4000,4000)),pages
benchmark=tasks[2]['steps'][0]['out']
assert re.search(r'"tasks_completed"\s*:\s*100000',benchmark),benchmark
assert 'canonical_checksums_verified' in tasks[3]['steps'][0]['out']
assert len(tasks[4]['steps'])==3 and all('PASS' in step['out'] or '0 failed' in step['out'] for step in tasks[4]['steps'])
groups=[task for task in evidence['tasks'] if 'group' in task]
assert len(groups)==1
assert [msg['sender'] for msg in groups[0]['messages'] if msg['role']=='assistant']==['reviewer','tester']
assert evidence['file_verification']=='hello — verified after live demo'
for name in ['README.md','README.zh-CN.md','backend/docs/runtime-benchmark-expanded.md']:
    document=docs.parents[1]/name
    text=document.read_text(encoding='utf-8')
    assert 'runtime-demo.mp4' in text and 'runtime-demo-2026-10-14.webm' not in text
print('PASS: 5-minute video hash/metadata, six real completed tasks, paginated source, 100k workload, group replies and final file')
