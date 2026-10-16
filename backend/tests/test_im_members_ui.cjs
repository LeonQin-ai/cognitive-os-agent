const {chromium}=require('playwright');
const fs=require('node:fs');
const path=require('node:path');
const assert=require('node:assert/strict');

(async()=>{
  const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER || 'msedge'});
  try {
    const page=await browser.newPage({viewport:{width:1280,height:900}});
    const errors=[]; page.on('pageerror',e=>errors.push(e.message));
    let agents=[], posts=[], updates=[], fail=false, failSend=true, failStop=false, nextTask=0;
    const tasks=new Map(), stopped=[];
    let sessions=[{id:11,name:'empty',kind:'group',members:[],discussion_rounds:2,messages:0}];
    await page.route('http://ui.test/**',async route=>{
      const req=route.request(), url=new URL(req.url()); let data=[],status=200;
      if(url.pathname==='/') return route.fulfill({contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
      if(url.pathname==='/v1/agents') data={agents};
      if(url.pathname==='/v1/im/channels') data={channels:[]};
      if(url.pathname==='/v1/im/sessions'){
        if(req.method()==='POST'){
          const body=req.postDataJSON(); posts.push(body);
          if(fail){status=400;data={error:'agent missing'};}
          else {data={id:12};sessions.push({...body,id:12,messages:0});}
        } else data={sessions};
      }
      if(url.pathname==='/v1/im/sessions/11/members'){
        const body=req.postDataJSON(); updates.push(body);
        if(fail){status=400;data={error:'save failed'};}
        else {Object.assign(sessions[0],body);data={ok:true};}
      }
      if(url.pathname.endsWith('/messages') && req.method()==='POST'){
        if(failSend){status=409;data={error:'choose agents first'};}
        else {const task_id=nextTask++;tasks.set(task_id,{status:'QUEUED'});data={ok:true,task_id};}
      }
      const task=url.pathname.match(/^\/v1\/tasks\/(\d+)$/);
      if(task){
        const id=Number(task[1]);data=tasks.get(id)||{};
        if(req.method()==='DELETE'){
          stopped.push(id);
          if(failStop){status=409;data={error:'retry stop'};}
          else {tasks.set(id,{status:'CANCELLED'});data={ok:true};}
        }
      }
      return route.fulfill({status,contentType:'application/json',body:JSON.stringify(data)});
    });
    await page.goto('http://ui.test/');
    await page.evaluate(async()=>{switchPanel('im');await renderIm();});
    await page.locator('#im-group').check();
    assert.equal(await page.locator('#im-rounds').inputValue(),'3');
    assert.match(await page.locator('#im-members').innerText(),/暂无已注册/);
    await page.locator('#im-create').click();
    assert.equal(posts.length,0);
    assert.match(await page.locator('#im-member-status').innerText(),/至少选择/);
    agents=[{name:'alpha',role:'源码分析'},{name:'beta',role:'验证'}];
    await page.evaluate(()=>imLoadAgents());
    await page.locator('#im-new').fill('new room');
    await page.locator('#im-members input[value="alpha"]').check();
    await page.locator('#im-rounds').selectOption('5');
    fail=true; await page.locator('#im-create').click();
    await page.waitForFunction(()=>$('im-member-status').textContent==='agent missing');
    assert.equal(await page.locator('#im-new').inputValue(),'new room');
    assert(await page.locator('#im-members input[value="alpha"]').isChecked());
    assert.equal(await page.locator('#im-rounds').inputValue(),'5');
    fail=false; await page.locator('#im-create').click();
    await page.waitForFunction(()=>IM_ACTIVE===12);
    assert.deepEqual(posts.at(-1).members,['alpha']);
    assert.equal(posts.at(-1).discussion_rounds,5);
    await page.getByRole('button',{name:'编辑 empty 的成员',exact:true}).click();
    assert.equal(await page.locator('#im-rounds').inputValue(),'2');
    await page.locator('#im-rounds').selectOption('4');
    await page.locator('#im-members input[value="alpha"]').check();
    await page.locator('#im-members input[value="beta"]').check();
    fail=true; await page.getByRole('button',{name:'保存成员',exact:true}).click();
    await page.waitForFunction(()=>$('im-member-status').textContent==='save failed');
    assert(await page.locator('#im-members input[value="beta"]').isChecked());
    assert.equal(await page.locator('#im-rounds').inputValue(),'4');
    fail=false; await page.getByRole('button',{name:'保存成员',exact:true}).click();
    await page.waitForFunction(()=>IM_EDITING===null);
    assert.deepEqual(updates.at(-1).members,['alpha','beta']);
    assert.equal(updates.at(-1).discussion_rounds,4);
    await page.evaluate(()=>imSelect(11));
    await page.locator('#im-input').fill('unsent message');
    await page.evaluate(()=>imSend());
    assert.equal(await page.locator('#im-input').inputValue(),'unsent message');
    assert.match(await page.locator('#im-run-status').innerText(),/choose agents/);

    // Queue immediately from the HTTP acknowledgement, including task ID zero.
    // No WebSocket is connected; polling must still observe terminal states.
    failSend=false;
    await page.evaluate(()=>imSend());
    assert.equal(await page.locator('#im-input').inputValue(),'');
    assert.match(await page.locator('#im-run-status').innerText(),/已排队/);
    assert(await page.locator('#im-stop').isVisible());
    assert.equal(await page.evaluate(()=>IM_SESSION_TASK.get(11)),0);
    const statusBox=await page.locator('#im-run-status').boundingBox(), thread=await page.locator('#im-thread').boundingBox();
    assert(statusBox.x<thread.x,'discussion status remains in the left column');
    await page.evaluate(()=>{
      imRunUpdate({task_id:0,session_id:11,status:'running',agent:'alpha',discussion_round:2,discussion_rounds:4});
      imRunUpdate({task_id:0,session_id:11,status:'queued'});
      imRunUpdate({task_id:0,session_id:11,status:'running',agent:'beta',discussion_round:1,discussion_rounds:4});
    });
    assert.match(await page.locator('#im-run-status').innerText(),/第 2\/4 轮 · alpha/);
    await page.locator('#im-input').fill('new topic');await page.evaluate(()=>imSend());
    await page.evaluate(()=>{
      imRunUpdate({task_id:0,session_id:11,status:'done',reason:'round_limit'});
      imRunUpdate({task_id:0,session_id:11,status:'cancelled',reason:'new_message'});
    });
    assert.match(await page.locator('#im-run-status').innerText(),/已排队/,'old completion cannot replace the newer discussion');
    await page.locator('#im-stop').click();
    await page.waitForFunction(()=>!$('im-stop').hidden&&$('im-stop').disabled);
    assert.deepEqual(stopped,[1]);
    await page.waitForFunction(()=>$('im-run-status').textContent==='讨论已停止');
    assert(await page.locator('#im-stop').isHidden());

    await page.locator('#im-input').fill('seek consensus');await page.evaluate(()=>imSend());
    await page.evaluate(()=>imRunUpdate({task_id:2,session_id:11,status:'done',reason:'consensus'}));
    assert.match(await page.locator('#im-run-status').innerText(),/全员已达成共识/);
    await page.evaluate(()=>{imSelect(12);imSelect(11);});
    assert.match(await page.locator('#im-run-status').innerText(),/全员已达成共识/);
    await page.locator('#im-input').fill('poll timeout');await page.evaluate(()=>imSend());
    tasks.set(3,{status:'TIMEOUT',im_status:'running'});
    await page.waitForFunction(()=>$('im-run-status').textContent.includes('执行超时'));

    await page.locator('#im-input').fill('stop retry');await page.evaluate(()=>imSend());
    await page.evaluate(()=>imRunUpdate({task_id:4,session_id:11,status:'running',agent:'beta',discussion_round:3,discussion_rounds:4}));
    failStop=true;await page.locator('#im-stop').click();
    await page.waitForFunction(()=>$('im-run-status').textContent==='retry stop');
    assert(await page.locator('#im-stop').isEnabled());
    failStop=false;
    await page.setViewportSize({width:390,height:844});
    await page.getByRole('button',{name:'编辑 empty 的成员',exact:true}).click();
    const box=await page.locator('#im-members').boundingBox();
    assert(box && box.width>0 && box.x>=0 && box.x+box.width<=390,JSON.stringify(box));
    for(const selector of ['#im-run-status','#im-stop','#im-rounds']){
      const box=await page.locator(selector).boundingBox();
      assert(box&&box.width>0&&box.x>=0&&box.x+box.width<=390,selector+': '+JSON.stringify(box));
    }
    await page.locator('#im-stop').click();
    await page.waitForFunction(()=>$('im-run-status').textContent==='讨论已停止');
    assert.deepEqual(stopped,[1,4,4]);
    await page.evaluate(()=>{
      imRunUpdate({task_id:5,session_id:11,status:'done',reason:'round_limit'});
      imRunUpdate({task_id:5,session_id:11,status:'queued'});
    });
    assert.match(await page.locator('#im-run-status').innerText(),/已完成设定轮数/);
    assert.deepEqual(errors,[]);
    console.log('PASS: group roster/round selection, atomic save error retention, task zero, HTTP polling, role/round status, stale-event isolation, stop/retry, consensus, timeout and 390px visibility');
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
