/* Five-minute local evidence walkthrough; actual small test runs are captured. */
const {chromium}=require('playwright');
const fs=require('node:fs'),path=require('node:path'),os=require('node:os'),{spawn}=require('node:child_process');
const root=path.resolve(__dirname,'..'),docs=path.join(root,'docs'),build=path.join(root,'build');
const take=path.join(build,'runtime-video-take');fs.mkdirSync(take,{recursive:true});
const scale=JSON.parse(fs.readFileSync(path.join(docs,'runtime-scale-2026-10-14.json')));
const pressure=JSON.parse(fs.readFileSync(path.join(docs,'runtime-context-pressure-2026-10-14.json')));
const crash=JSON.parse(fs.readFileSync(path.join(docs,'runtime-crash-recovery-2026-10-14.json')));
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
function safeRemove(dir,parent,prefix='') {const abs=path.resolve(dir),base=path.resolve(parent);if(!abs.startsWith(base+path.sep)||!path.basename(abs).startsWith(prefix))throw new Error('cleanup outside owned area');fs.rmSync(abs,{recursive:true,force:true});}
(async()=>{
 const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});
 const ctx=await browser.newContext({viewport:{width:1280,height:720},recordVideo:{dir:take,size:{width:1280,height:720}}});
 const page=await ctx.newPage(),video=page.video();
 await page.setContent(`<!doctype html><meta charset="utf-8"><style>*{box-sizing:border-box}body{margin:0;background:#eef2f7;color:#16233b;font:18px 'Segoe UI','Microsoft YaHei',sans-serif}.shell{padding:34px 48px}header{display:flex;justify-content:space-between;color:#526684;font-size:16px}h1{font-size:35px;line-height:1.2;margin:26px 0 12px}#sub{color:#526684;line-height:1.7}.cards{display:flex;gap:18px;margin:22px 0}.card{background:white;border-radius:14px;border:1px solid #dce4ee;padding:18px;flex:1}.num{font-size:34px;font-weight:650;color:#30697c}.label{color:#526684;font-size:15px;margin-top:6px}pre{margin:0;background:#172438;color:#c8e7de;border-radius:12px;padding:20px;white-space:pre-wrap;overflow:hidden;height:263px;font:15px/1.5 Consolas,monospace}footer{margin-top:18px;font-size:14px;color:#526684}.progress{height:4px;background:#217977;position:fixed;bottom:0;left:0;transition:width 1s linear}</style><div class="shell"><header><span>COGNITIVE OS · RUNTIME VERIFICATION</span><span id="clock">00:00 / 05:00</span></header><h1 id="title"></h1><div id="sub"></div><div id="cards" class="cards"></div><pre id="terminal"></pre><footer>实测与本地确定性 fixture；不代表生产模型吞吐。完整数据、源码、复现命令随报告保留。</footer></div><div class="progress" id="bar"></div>`);
 const start=Date.now();await page.evaluate(()=>{window.start=Date.now();setInterval(()=>{const s=Math.min(300,Math.floor((Date.now()-window.start)/1000));document.getElementById('clock').textContent=String(Math.floor(s/60)).padStart(2,'0')+':'+String(s%60).padStart(2,'0')+' / 05:00';document.getElementById('bar').style.width=(s/3)+'%'},1000)});
 async function scene(title,sub,cards,text){await page.evaluate(({title,sub,cards,text})=>{document.getElementById('title').textContent=title;document.getElementById('sub').textContent=sub;const box=document.getElementById('cards');box.replaceChildren();for(const [num,label] of cards){const c=document.createElement('div');c.className='card';const n=document.createElement('div');n.className='num';n.textContent=num;const l=document.createElement('div');l.className='label';l.textContent=label;c.append(n,l);box.append(c)}document.getElementById('terminal').textContent=text},{title,sub,cards,text})}
 async function until(sec){const left=start+sec*1000-Date.now();if(left>0)await sleep(left)}
 async function run(exe,args){await page.evaluate(t=>document.getElementById('terminal').textContent=t,'$ '+exe+' '+args.join(' ')+'\n');return await new Promise((resolve,reject)=>{let text='';const child=spawn(exe,args,{cwd:root,windowsHide:true});for(const stream of [child.stdout,child.stderr])stream.on('data',chunk=>{text+=chunk.toString();page.evaluate(t=>document.getElementById('terminal').textContent=t,takeTail(text)).catch(()=>{})});child.on('error',reject);child.on('exit',code=>code===0?resolve(text):reject(new Error(text)));});}
 function takeTail(text){return text.length>2600?'[earlier output omitted from video; retained test/report has full data]\n'+text.slice(-2600):text}
 const tmp=fs.mkdtempSync(path.join(os.tmpdir(),'coa-runtime-demo-'));
 try {
 await scene('可复现的 Runtime，而不是预设分数','2026-10-14 · Windows / Zig cc -O1 · 规模测试每组3次；内存压力与崩溃各标明样本量。',[['1000','注册角色'],['100,000','完成任务'],['8','测量负载 Worker']], '这段录像展示已有完整实测，并执行小规模本地回归。\n\n注册 Agent 数 ≠ 同时运行的进程数。\n调度吞吐 ≠ 生产 LLM 推理吞吐。\nContext 正文 / MMU 缓存 / 模型输入 token 分开统计。');
 await page.screenshot({path:path.join(docs,'runtime-demo-cover.png')});await until(45);
 const runs=scale.runs.filter(r=>r.agents===1000&&r.tasks_completed===100000).sort((a,b)=>a.elapsed_ms-b.elapsed_ms),r=runs[1];
 await scene('1 · 调度扩展与队列延迟','完整实测：100/1000角色 × 1万/10万任务；固定64次Context探测。下面实际运行1000角色/1000任务小用例。',[[r.elapsed_ms.toFixed(1)+' ms','10万任务中位耗时'],[r.scheduler_latency_p99_ms.toFixed(3)+' ms','该次排队P99'],['0','丢失 / 拒绝 / 结束队列']], '启动本地正确性验证…');
 await run(path.join(build,'bench-runtime.exe'),[tmp,'1000','4','8','1000','8']);await until(105);
 await scene('2 · Context 压力与真实恢复','完整压力：512/128/10 MiB 和 768/128/128 MiB。L1概览有损，规范正文必须保留；下面执行4MiB缩小用例。',[['650 MiB','混合正文案例'],['1 GiB','独立聚合案例'],['3 → 0 → 3','HOT / 驱逐 / HOT']], '启动真实Context MMU序列…');
 await run(path.join(build,'bench-runtime.exe'),['--context',tmp,'2','1','1']);await until(165);
 await scene('3 · 强制崩溃 → 恢复 → 显式重放','Worker是进程内线程：测试强制结束整个临时Runtime进程，并重启相同状态目录。不会自动重放任意外部操作。',[[crash.restart_to_detect_ms.toFixed(1)+' ms','报告中的重启检测'],[crash.crash_to_done_ms.toFixed(1)+' ms','报告中的崩溃至完成'],['0 → 1 → 2','旧任务 / 重放 / 后续任务']], '现在执行真实强制终止测试…');
 await run(process.env.DEMO_PYTHON||'python',['tests/test_crash_recovery_e2e.py']);await until(245);
 await scene('4 · 证据、边界与复现入口','完整数据写入README；最终JSON、报告与用例保留。临时状态、重复日志和绘图环境清理。',[['3','规模重复次数'],['100%','两组规范正文hash核对通过'],['显式','恢复与重放策略']], 'tests/run_runtime_measurements.py --full --output docs\n'+'tests/test_runtime_benchmark.py\n'+'tests/test_crash_recovery_e2e.py --report\n\n'+ '保留：README / 原始JSON / Benchmark报告 / 白皮书与3张架构图。\n'+'边界：有损概览不是无损压缩；幂等文件测试不证明任意外部操作exactly-once。');
 await until(300);
 } finally {safeRemove(tmp,os.tmpdir(),'coa-runtime-demo-');await ctx.close();}
 const output=path.join(docs,'runtime-demo-2026-10-14.webm');await video.saveAs(output);
 const qa=await browser.newPage();const source='data:video/webm;base64,'+fs.readFileSync(output).toString('base64');
 const duration=await qa.evaluate(src=>new Promise((resolve,reject)=>{const v=document.createElement('video');v.src=src;v.onloadedmetadata=()=>resolve(v.duration);v.onerror=()=>reject(new Error('video decode failed'));document.body.append(v)}),source);
 if(!Number.isFinite(duration)||duration<299||duration>315)throw new Error('Unexpected duration '+duration);
 await browser.close();safeRemove(take,build,'runtime-video-take');console.log('PASS: recorded walkthrough duration='+duration.toFixed(2)+'s bytes='+fs.statSync(output).size);
})().catch(e=>{console.error(e);process.exitCode=1});
