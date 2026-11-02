"""Actual Docker build/run isolation and persistence smoke test; no paid API."""
import json,subprocess,time,urllib.request,uuid
from pathlib import Path
root=Path(__file__).resolve().parents[2];tag='coa-smoke-'+uuid.uuid4().hex[:12]
def docker(*args):return subprocess.check_output(['docker',*args],cwd=root,text=True).strip()
volumes=[tag+'-state',tag+'-work'];container=None
try:
 subprocess.run(['docker','build','-t',tag,'.'],cwd=root,check=True)
 for volume in volumes:docker('volume','create',volume)
 container=docker('run','-d','--name',tag,'--read-only','--cap-drop=ALL','--security-opt=no-new-privileges:true','--pids-limit=256','--memory=2g','--cpus=2','--tmpfs','/tmp:rw,size=128m','-p','127.0.0.1::18300','-v',volumes[0]+':/state','-v',volumes[1]+':/workspace',tag)
 port=json.loads(docker('inspect',container))[0]['NetworkSettings']['Ports']['18300/tcp'][0]['HostPort']
 def wait():
  for _ in range(100):
   try:
    with urllib.request.urlopen(f'http://127.0.0.1:{port}/v1/scheduler',timeout=2) as r:return json.load(r)
   except OSError:time.sleep(.2)
  raise AssertionError('container HTTP startup timeout')
 wait()
 assert docker('exec',container,'id','-u')=='10001'
 inspection=json.loads(docker('inspect',container))[0]
 assert inspection['HostConfig']['ReadonlyRootfs'] and inspection['HostConfig']['CapDrop']==['ALL']
 docker('exec',container,'python3','-c',"from pathlib import Path;Path('/workspace/proof').write_text('persisted');Path('/state/proof').write_text('retained');assert not Path('/src').exists()")
 denied=subprocess.run(['docker','exec',container,'python3','-c',"open('/etc/coa-write-test','w').write('bad')"],capture_output=True)
 assert denied.returncode!=0,'root filesystem writable'
 docker('restart',container);wait()
 docker('exec',container,'python3','-c',"from pathlib import Path;assert Path('/workspace/proof').read_text()=='persisted';assert Path('/state/proof').read_text()=='retained'")
 print('PASS: non-root, read-only root, capabilities dropped, HTTP access, persistent state/workspace and restart')
finally:
 if container:
  print(docker('logs',container));docker('rm','-f',container)
 for volume in volumes:subprocess.run(['docker','volume','rm',volume],capture_output=True)
 subprocess.run(['docker','image','rm',tag],capture_output=True)
