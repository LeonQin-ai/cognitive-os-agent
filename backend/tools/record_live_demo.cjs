/* Continuous real console recording. Local deterministic model; real runtime/tools.
 * NODE_PATH=webarena/node_modules node tools/record_live_demo.cjs [--smoke]
 * --smoke validates the same flow quickly; only the default 300s take is deliverable. */
const {chromium}=require('playwright');
const fs=require('node:fs'),path=require('node:path'),os=require('node:os'),http=require('node:http');
const {spawn,execFileSync}=require('node:child_process');
const crypto=require('node:crypto');
const root=path.resolve(__dirname,'..'),docs=path.join(root,'docs'),build=path.join(root,'build');
const smoke=process.argv.includes('--smoke'),seconds=smoke?45:300;
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const quote=p=>'"'+p.replaceAll('\\','/')+'"';
const binary=path.join(build,'cognitive-os-agent'+(process.platform==='win32'?'.exe':''));
const bench=path.join(build,'bench-runtime'+(process.platform==='win32'?'.exe':''));
const native=path.join(build,'cognitive-os-agent-test'+(process.platform==='win32'?'.exe':''));
const take=fs.mkdtempSync(path.join(build,'live-demo-take-'));
const state=fs.mkdtempSync(path.join(os.tmpdir(),'coa-live-demo-'));
const scenarios=new Map(),evidence=[];let runtime,browser,context;
const source=fs.readFileSync(path.join(root,'src/cognition/planner.c'),'utf8');
const excerpt=source.slice(source.indexOf('/* DSML is sometimes'),source.indexOf('/* Shared planning core.'));
fs.writeFileSync(path.join(state,'planner-dsml.c'),excerpt);
const dsml=actions=>'<｜｜DSML｜｜ calls>'+actions.map(a=>'<｜｜DSML｜｜ invoke name="'+a.tool+'">'+
  '<｜｜DSML｜｜ parameter name="args" string="false">'+JSON.stringify(a.args)+
  '</｜｜DSML｜｜ parameter></｜｜DSML｜｜ invoke>').join('')+'</｜｜DSML｜｜ calls>';
const act=(tool,args)=>({tool,args});
const provider=http.createServer(async(req,res)=>{
  try {
    let body='';for await(const chunk of req)body+=chunk;
    const text=JSON.parse(body).messages.at(-1).content;
    const current=text.split('## Current request\n').at(-1).split('\n## ')[0];
    const marker=(current.match(/LIVE_[A-Z]+/)||[])[0];
    const round=scenarios.get(marker)||0;scenarios.set(marker,round+1);
    let answer;
    if(current.includes('in an IM group')){
      const name=(current.match(/You are (\S+)/)||[])[1];
      answer=name==='reviewer'?'我的职责是讨论技术方案和设计边界。本群正在演示注册成员依次回应；内容由本地确定性测试模型提供。':'我的职责是讨论验收标准。群成员保存后下一轮生效；每条用户消息触发一轮，每位 Agent 回应一次。';
    } else if(marker==='LIVE_WRITE'){
      answer=round===0?dsml([act('list_dir',{path:'.'}),act('file_write',{path:'demo-note.txt',content:'hello from real runtime'})]):
        round===1?JSON.stringify([act('file_read',{path:'demo-note.txt'})]):'已通过真实工具创建并回读 demo-note.txt，内容为 hello from real runtime。展开详细执行记录可核对实际工具输出。';
    } else if(marker==='LIVE_SOURCE'){
      answer=round<Math.ceil(Buffer.byteLength(excerpt)/4000)?JSON.stringify([act('file_read',{path:'planner-dsml.c',offset_bytes:round*4000,max_bytes:4000})]):
        '源码节选已连续分页读完：dsml_tag_read 识别标签和属性；parse_dsml_tool_calls 将参数转换为 planned_action。非法或截断 DSML 整批拒绝，普通文本和 JSON 中的示例不会作为 DSML 执行。';
    } else if(marker==='LIVE_BENCH'){
      answer=round===0?JSON.stringify([act('shell',{command:quote(bench)+' '+quote(path.join(state,'bench'))+' '+(smoke?1000:100000)+' 4 8 1000 8',timeout_ms:60000})]):
        '调度基准已实际执行。请展开 shell 输出核对 tasks_submitted/admitted/completed、失败/拒绝数、队列归零和 rollback_verified。此处是确定性 Runtime 负载，不是生产模型吞吐。';
    } else if(marker==='LIVE_CONTEXT'){
      answer=round===0?JSON.stringify([act('shell',{command:quote(bench)+' --context '+quote(path.join(state,'context'))+' 2 1 1',timeout_ms:60000})]):
        '真实 Context MMU 序列已执行：HOT → 驱逐 → WARM 概述 → HOT 恢复。请在输出中核对 canonical_checksums_verified；概述有损，规范原文完整保留。';
    } else if(marker==='LIVE_VERIFY'){
      const commands=[quote(native)+' --planner','python '+quote(path.join(root,'tests/test_im_members_e2e.py')),'python '+quote(path.join(root,'tests/test_crash_recovery_e2e.py'))];
      answer=round<commands.length?JSON.stringify([act('shell',{command:commands[round],timeout_ms:60000})]):
        '验证命令已执行：规划器、群成员保存与重启、独立临时 Runtime 进程强制终止及显式恢复。请以每个 shell 的实际 PASS 输出为准。崩溃测试仅结束它自己创建的临时子进程。';
    } else if(marker==='LIVE_EDIT'){
      answer=round===0?JSON.stringify([act('file_edit',{path:'demo-note.txt',old_string:'hello from real runtime',new_string:'hello — verified after live demo'})]):
        round===1?JSON.stringify([act('file_read',{path:'demo-note.txt'})]):'文件已精确修改并回读核对：hello — verified after live demo。所有操作已进入实际任务日志。';
    } else answer='本地演示模型已就绪。';
    await sleep(smoke?100:1500);
    const data=JSON.stringify({choices:[{message:{role:'assistant',content:answer},finish_reason:'stop'}]});
    res.writeHead(200,{'Content-Type':'application/json','Content-Length':Buffer.byteLength(data)});res.end(data);
  } catch(error){res.writeHead(500);res.end(JSON.stringify({error:String(error)}));}
});

(async()=>{
  let success=false;
  try {
    await new Promise(resolve=>provider.listen(0,'127.0.0.1',resolve));
    fs.writeFileSync(path.join(state,'cognitive-os-agent.json'),JSON.stringify({'llm.provider':'openai',
      'llm.model':'local-demo-fixture','llm.api_key':'local-test-only','llm.base_url':'http://127.0.0.1:'+provider.address().port,
      'reasoning.max_rounds':12,'scheduler.workers':2}));
    const reservation=http.createServer();await new Promise(resolve=>reservation.listen(0,'127.0.0.1',resolve));
    const port=reservation.address().port;await new Promise(resolve=>reservation.close(resolve));
    const base='http://127.0.0.1:'+port;
    const log=fs.openSync(path.join(take,'runtime.log'),'w');
    runtime=spawn(binary,['--state',state,'--workspace',state,'serve',String(port)],{cwd:root,windowsHide:true,stdio:['ignore',log,log]});fs.closeSync(log);
    async function api(method,url,body){
      const r=await fetch(base+url,{method,headers:{'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined});
      if(!r.ok)throw Error(url+' '+r.status+' '+await r.text());return r.json();
    }
    for(let i=0;;i++){try{await api('GET','/v1/scheduler');break;}catch(error){if(i>200)throw error;await sleep(100);}}
    browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});
    context=await browser.newContext({viewport:{width:1440,height:900},recordVideo:{dir:take,size:{width:1440,height:900}}});
    const page=await context.newPage(),video=page.video();
    await page.goto(base);await page.waitForSelector('#chat-input:not([disabled])');
    await page.evaluate(()=>{
      applyTheme('light');document.body.style.marginTop='0';document.body.style.paddingTop='52px';
      document.body.style.boxSizing='border-box';document.body.style.height='100vh';
      document.querySelectorAll('aside,main').forEach(el=>el.style.height='100%');
      const bar=document.createElement('div');bar.id='live-recording-caption';
      bar.style.cssText='position:fixed;top:0;left:0;right:0;height:52px;background:#112437;color:white;z-index:99999;padding:7px 18px;font:14px/19px Microsoft YaHei,sans-serif;display:flex;justify-content:space-between';
      const title=document.createElement('span');title.id='live-chapter';title.textContent='Cognitive OS · 真实应用连续实操';
      const clock=document.createElement('span');clock.id='live-clock';bar.append(title,clock);document.body.append(bar);
      window.liveStart=Date.now();setInterval(()=>{const s=Math.floor((Date.now()-liveStart)/1000);clock.textContent=String(Math.floor(s/60)).padStart(2,'0')+':'+String(s%60).padStart(2,'0')+' ｜ 本地确定性测试模型 · 真实 Runtime / 工具执行'},250);
    });
    const start=Date.now();
    async function caption(text){console.log(text);await page.locator('#live-chapter').evaluate((el,t)=>el.textContent=t,text);}
    async function until(second){const target=start+second*1000*seconds/300;while(Date.now()<target)await sleep(Math.min(500,target-Date.now()));}
    async function inspect(){
      const summaries=page.locator('.run-details > summary');
      for(let i=Math.max(0,await summaries.count()-2);i<await summaries.count();i++){
        if(!await summaries.nth(i).evaluate(el=>el.parentElement.open))await summaries.nth(i).click();
      }
      const detail=page.locator('.run-step-detail > summary').last();
      if(await detail.count() && !await detail.evaluate(el=>el.parentElement.open))await detail.click();
      await page.evaluate(()=>{const el=Array.from(document.querySelectorAll('.run-step-detail')).at(-1);if(el)el.scrollIntoView({block:'center'});});
    }
    async function chat(prompt){
      await page.evaluate(async()=>{switchPanel('chat');await chatNewSession();});
      await page.locator('#chat-input').fill(prompt);
      const submitted=page.waitForResponse(r=>r.request().method()==='POST' && new URL(r.url()).pathname==='/v1/chat');
      await page.locator('#chat-send').click();const queued=await (await submitted).json();
      const deadline=Date.now()+90000;let task;
      do{await sleep(300);task=await api('GET','/v1/tasks/'+queued.id);if(Date.now()>deadline)throw Error('task timeout');}while(!['DONE','FAILED','CANCELLED','TIMEOUT'].includes(task.status));
      if(task.status!=='DONE'||!task.steps?.length||task.steps.some(s=>!s.ok))throw Error(JSON.stringify(task));
      evidence.push(task);await sleep(500);await inspect();return task;
    }
    await caption('01 / DSML 工具计划 → 真实列目录、写文件与回读');
    await chat('LIVE_WRITE 创建 demo-note.txt，内容 hello from real runtime，然后回读核验。');
    if(fs.readFileSync(path.join(state,'demo-note.txt'),'utf8')!=='hello from real runtime')throw Error('write mismatch');
    await page.screenshot({path:path.join(docs,'runtime-demo-cover.png')});await until(30);
    await caption('02 / 读取当前项目的 DSML 源码节选 · 连续分页直到 EOF');
    await chat('LIVE_SOURCE 完整读取 planner-dsml.c 的全部源码节选，说明解析和失败边界。');await until(75);
    await caption('03 / 注册两位 Agent → 创建群聊 → 双 Agent 顺序回应');
    await page.evaluate(()=>switchPanel('agents'));
    for(const [name,role] of [['reviewer','讨论技术方案'],['tester','讨论验收标准']]){
      await page.locator('#agent-name').fill(name);await page.locator('#agent-role').fill(role);
      const posted=page.waitForResponse(r=>r.request().method()==='POST' && new URL(r.url()).pathname==='/v1/agents');
      await page.getByRole('button',{name:'Add',exact:true}).click();if(!(await posted).ok())throw Error('register failed');await sleep(smoke?100:2500);
    }
    await page.evaluate(async()=>{switchPanel('im');await renderIm();});
    await page.locator('#im-group').check();await page.locator('#im-new').fill('技术审查群');
    for(const name of ['reviewer','tester'])await page.locator('#im-members input[value="'+name+'"]').check();
    const created=page.waitForResponse(r=>r.request().method()==='POST' && new URL(r.url()).pathname==='/v1/im/sessions');
    await page.locator('#im-create').click();const room=(await (await created).json()).id;
    await page.locator('#im-input').fill('请每人简短介绍自己的职责。');await page.evaluate(()=>imSend());
    for(let i=0;;i++){const messages=await api('GET','/v1/im/sessions/'+room+'/messages');if(messages.filter(m=>m.role==='assistant').length===2){evidence.push({group:room,messages});break;}if(i>100)throw Error('missing group replies');await sleep(300);}
    await sleep(smoke?250:1500);await page.evaluate(()=>renderIm());await until(125);
    await caption('04 / 实际运行：1000 Agent / '+(smoke?'1000':'100000')+' Task · 展开 shell 查看计数与延迟');
    await chat('LIVE_BENCH 运行本地 Runtime 调度基准并展示真实计数、耗时与回滚核对结果。');await until(170);
    await caption('05 / 实际运行 Context MMU：HOT → 驱逐 → WARM → 原文恢复与 hash 核对');
    await chat('LIVE_CONTEXT 执行4 MiB Context压力验证并展示实际输出。');await until(215);
    await caption('06 / 实际执行测试：规划器、群聊重启持久化、进程崩溃与显式恢复');
    await chat('LIVE_VERIFY 执行规划器、IM群成员和崩溃恢复三条真实回归命令，展示实际PASS结果。');await until(260);
    await caption('07 / 精确修改文件 → 回读核验 → 查看真实任务历史与轨迹');
    await chat('LIVE_EDIT 修改 demo-note.txt，把 hello from real runtime 精确替换为 hello — verified after live demo，然后回读。');
    const finalText=fs.readFileSync(path.join(state,'demo-note.txt'),'utf8');if(finalText!=='hello — verified after live demo')throw Error('edit mismatch');
    await until(277);await page.evaluate(()=>switchPanel('snapshots'));
    await page.waitForSelector('#journal-list');await page.locator('#journal-list').scrollIntoViewIfNeeded();
    await until(288);await page.evaluate(()=>switchPanel('trace'));
    await page.waitForSelector('#trace-history');await until(296);
    await caption('实操完成：工具输出、文件内容、群聊回复和任务终态均已核验 · 录像原速连续');
    await until(300);await context.close();context=null;
    const raw=path.join(take,'continuous.webm');await video.saveAs(raw);
    const ffmpeg=process.env.DEMO_FFMPEG||execFileSync('python',['-c','import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())'],{encoding:'utf8'}).trim();
    const output=path.join(smoke?take:docs,'runtime-demo.mp4');
    execFileSync(ffmpeg,['-y','-i',raw,'-an','-c:v','libx264','-preset','fast','-crf','25','-r','15','-pix_fmt','yuv420p','-movflags','+faststart',output],{stdio:'ignore',windowsHide:true});
    const qa=await browser.newPage();await qa.goto(base);
    await qa.route('**/recording.mp4',route=>route.fulfill({path:output,contentType:'video/mp4'}));
    const meta=await qa.evaluate(()=>new Promise((resolve,reject)=>{const v=document.createElement('video');v.src='/recording.mp4';v.onloadedmetadata=()=>resolve({duration:v.duration,width:v.videoWidth,height:v.videoHeight});v.onerror=()=>reject(Error('decode failed'));document.body.append(v);}));
    if(!Number.isFinite(meta.duration)||meta.duration<seconds||meta.duration>seconds+30)throw Error('duration '+meta.duration);
    if(!smoke)fs.writeFileSync(path.join(docs,'runtime-demo-evidence.json'),JSON.stringify({
      recorded_at:new Date().toISOString(),commit:execFileSync('git',['rev-parse','HEAD'],{cwd:root,encoding:'utf8'}).trim(),
      recording:'continuous real console; original speed; silent with Chinese captions',model:'local deterministic fixture; no production model throughput claim',
      ...meta,bytes:fs.statSync(output).size,sha256:crypto.createHash('sha256').update(fs.readFileSync(output)).digest('hex'),
      source_excerpt_sha256:crypto.createHash('sha256').update(excerpt).digest('hex'),file_verification:finalText,
      tasks:evidence.map(t=>t.group?t:{id:t.id,status:t.status,tool_calls:t.tool_calls,steps:t.steps})},null,2));
    console.log('PASS: '+JSON.stringify({...meta,bytes:fs.statSync(output).size,tasks:evidence.length,smoke}));success=true;
  } finally {
    if(context)await context.close();if(browser)await browser.close();
    if(runtime){runtime.kill();await new Promise(resolve=>runtime.exitCode!==null?resolve():runtime.once('exit',resolve));}
    await new Promise(resolve=>provider.close(resolve));
    for(const [dir,parent,prefix] of [[state,os.tmpdir(),'coa-live-demo-'],[take,build,'live-demo-take-']]){
      const abs=path.resolve(dir),base=path.resolve(parent);if(!abs.startsWith(base+path.sep)||!path.basename(abs).startsWith(prefix))throw Error('unsafe cleanup');
      if(success)fs.rmSync(abs,{recursive:true,force:true});else console.error('Retained failed recording evidence:',abs);
    }
  }
})().catch(error=>{console.error(error);process.exitCode=1;});
