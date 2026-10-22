"""Actual owned Runtime subprocess crash, snapshot restore and explicit resume (#130/#122)."""
import json, os, socket, subprocess, tempfile, threading, time, urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from fixture_completion import reviewed_answer
entered=threading.Event(); release=threading.Event(); counts={}; writes=[]
class Provider(BaseHTTPRequestHandler):
    def log_message(self,*args): pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        text=body['messages'][-1]['content']; current=text.split('## Current request\n',1)[-1].split('\n## ',1)[0]
        if 'AFTER_RECOVERY_CASE' in current:
            n=counts.get('following',0); counts['following']=n+1
            if n==0:
                writes.append('CONTINUED'); answer=json.dumps([{'tool':'file_write','args':{'path':'after-recovery.txt','content':'CONTINUED'}}])
            else: answer='文件已生成。'
        else:
            n=counts.get('recovery',0); counts['recovery']=n+1
            if n==0:
                writes.append('PARTIAL_STAGE'); answer=json.dumps([{'tool':'file_write','args':{'path':'recover.txt','content':'PARTIAL_STAGE'}}])
            elif n==1:
                entered.set(); assert release.wait(60),'test did not release held request'; answer='等待中的任务。'
            elif n==2:
                writes.append('COMPLETE_STAGE'); answer=json.dumps([{'tool':'file_write','args':{'path':'recover.txt','content':'COMPLETE_STAGE'}}])
            else: answer='文件已生成。'
        answer=reviewed_answer(body,answer)
        data=json.dumps({'choices':[{'message':{'content':answer}}]}).encode()
        try:
            self.send_response(200); self.send_header('Content-Type','application/json')
            self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)
        except (BrokenPipeError,ConnectionResetError): pass
backend = Path(__file__).resolve().parents[1]
build_dir=backend/os.environ.get("COA_BUILD_DIR","build")
binary=build_dir/("cognitive-os-agent.exe" if os.name=="nt" else "cognitive-os-agent")
with tempfile.TemporaryDirectory(prefix='coa-crash-recovery-') as folder:
    state = Path(folder)
    provider = ThreadingHTTPServer(('127.0.0.1',0), Provider)
    threading.Thread(target=provider.serve_forever,daemon=True).start()
    (state/'cognitive-os-agent.json').write_text(json.dumps({'llm.provider':'openai','llm.model':'skill-test',
        'llm.base_url':f'http://127.0.0.1:{provider.server_port}','reasoning.max_rounds':6}),encoding='utf-8')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1',0)); port = sock.getsockname()[1]
    def request(path, body=None):
        req = urllib.request.Request(f'http://127.0.0.1:{port}'+path,
            data=json.dumps(body).encode() if body is not None else None,
            headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(req,timeout=5) as r: return json.load(r)
    with (build_dir/'crash-recovery-e2e.log').open('w',encoding='utf-8') as log:
        process = subprocess.Popen([str(binary),'--state',str(state),
            '--workspace',str(state),'serve',str(port)],cwd=backend,stdout=log,stderr=log,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        try:
            deadline = time.monotonic()+20
            while True:
                try: request('/v1/scheduler'); break
                except OSError:
                    assert process.poll() is None and time.monotonic()<deadline
                    time.sleep(.1)
            original='ORIGINAL_CHECKPOINT\n'; (state/'recover.txt').write_text(original)
            started=request('/v1/tasks',{'prompt':'创建 recover.txt 并完成 CRASH_RECOVERY_CASE'})
            assert started['id']==0,started
            assert entered.wait(15),'task never entered held model request'
            live=request('/v1/tasks/0'); assert live['status']=='RUNNING',live
            snapshots=request('/v1/snapshots'); assert snapshots,snapshots
            checkpoint=next(x for x in snapshots if any('recover.txt' in p for p in x['paths']))
            assert (state/'recover.txt').read_text()=='PARTIAL_STAGE'
            crash_at=time.perf_counter(); killed_pid=process.pid
            process.kill(); process.wait(timeout=5); release.set()
            # Restart exactly the same runtime state, without changing its journal.
            restarted_at=time.perf_counter()
            process=subprocess.Popen([str(binary),'--state',str(state),
                '--workspace',str(state),'serve',str(port)],cwd=backend,stdout=log,stderr=log,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            deadline=time.monotonic()+20
            while True:
                try:
                    journal=request('/v1/tasks/journal')
                    records=journal if isinstance(journal,list) else journal.get('tasks',journal.get('records',[]))
                    found=[x for x in records if x['id']==0 and x['status']=='INTERRUPTED']
                    if found: break
                except OSError: pass
                assert process.poll() is None and time.monotonic()<deadline
                time.sleep(.05)
            detected_at=time.perf_counter()
            assert not any(x['id']==0 and x['status']=='DONE' for x in records)
            restore_start=time.perf_counter()
            restored=request('/v1/snapshots/rollback',{'id':checkpoint['id'],'paths':checkpoint['paths']})
            assert restored['ok'] and (state/'recover.txt').read_text()==original,restored
            restored_at=time.perf_counter()
            try:
                request('/v1/tasks/not-a-number/resume',{})
                raise AssertionError('malformed task ID unexpectedly resumed task zero')
            except urllib.error.HTTPError as error:
                assert error.code==400,error
            replay=request('/v1/tasks/0/resume',{}); assert replay['id']>0,replay
            deadline=time.monotonic()+20
            while True:
                task=request('/v1/tasks/'+str(replay['id']))
                if task['status'] in ('DONE','FAILED','CANCELLED','TIMEOUT'): break
                assert time.monotonic()<deadline; time.sleep(.05)
            completed_at=time.perf_counter()
            assert task['status']=='DONE' and (state/'recover.txt').read_text()=='COMPLETE_STAGE',task
            following=request('/v1/tasks',{'prompt':'创建 after-recovery.txt AFTER_RECOVERY_CASE'})
            assert following['id']>replay['id'],following
            deadline=time.monotonic()+20
            while True:
                next_task=request('/v1/tasks/'+str(following['id']))
                if next_task['status'] in ('DONE','FAILED','CANCELLED','TIMEOUT'): break
                assert time.monotonic()<deadline; time.sleep(.05)
            assert next_task['status']=='DONE' and (state/'after-recovery.txt').read_text()=='CONTINUED',next_task
            assert writes==['PARTIAL_STAGE','COMPLETE_STAGE','CONTINUED'],writes
            report={'date':'2026-10-14','fault_boundary':'entire Runtime subprocess, not one in-process worker',
                'injection':'TerminateProcess' if __import__('os').name=='nt' else 'SIGKILL',
                'killed_pid':killed_pid,'restarted_pid':process.pid,'precondition_task_status':live['status'],
                'snapshot_id':checkpoint['id'],'snapshot_available':True,'interrupted_detected':True,
                'restart_to_detect_ms':(detected_at-restarted_at)*1000,'snapshot_restore_ms':(restored_at-restore_start)*1000,
                'manual_replay_to_done_ms':(completed_at-restored_at)*1000,'crash_to_done_ms':(completed_at-crash_at)*1000,
                'old_task_id':0,'replayed_task_id':replay['id'],'following_task_id':following['id'],
                'restore_content_verified':True,'replay_content_verified':True,'following_task_done':True,
                'replay_mode':'explicit resume after verified snapshot restore; idempotent file replacement; no automatic external operation replay',
                'duplicate_fixture_writes':0}
            if '--report' in __import__('sys').argv:
                (backend/'docs/runtime-crash-recovery-2026-10-14.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
            print('PASS: hard process kill while RUNNING + available snapshot -> INTERRUPTED -> verified Restore -> explicit Replay -> DONE -> Continue')
            print(json.dumps(report,ensure_ascii=False))
        finally:
            release.set(); process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
            provider.shutdown()
