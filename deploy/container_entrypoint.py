"""Seed container defaults without embedding credentials or replacing user settings."""
import json,os,sys
from pathlib import Path
state=Path('/state');config=state/'cognitive-os-agent.json'
data=json.loads(config.read_text(encoding='utf-8-sig')) if config.exists() else {}
# A container must listen on its virtual interface. Compose publishes loopback only.
data['http.bind']='0.0.0.0'
if 'llm.provider' not in data:data['llm.provider']='mock'
os.umask(0o077)
temporary=state/'cognitive-os-agent.json.tmp'
temporary.write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf8')
os.replace(temporary,config)
os.execvp('cognitive-os-agent',['cognitive-os-agent','--state','/state','--workspace','/workspace',*sys.argv[1:]])
