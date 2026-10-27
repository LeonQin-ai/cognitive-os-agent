const {chromium}=require('playwright');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
(async()=>{
 const browser=await chromium.launch({headless:true,...(process.env.UI_TEST_EXECUTABLE?{executablePath:process.env.UI_TEST_EXECUTABLE}:{channel:process.env.UI_TEST_BROWSER||'msedge'})});
 try{
  const page=await browser.newPage(),errors=[],stops=[];
  let state={status:'RUNNING',im_status:'running',stage:'waiting_for_executor',activity:'正在等待可用执行器',agent:'alpha',discussion_round:1,discussion_rounds:3};
  page.on('pageerror',e=>errors.push(e.message));
  await page.route('http://ui.test/**',async route=>{
   const r=route.request(),u=new URL(r.url());let body=[];
   if(u.pathname==='/')return route.fulfill({contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
   if(u.pathname==='/v1/im/sessions')body={sessions:[{id:1,kind:'group',members:['alpha'],discussion_rounds:3}]};
   if(u.pathname==='/v1/tasks/91'){
    if(r.method()==='DELETE'){stops.push(91);state={...state,status:'CANCELLED',im_status:'cancelled',stage:'cancelled'};body={ok:true};}
    else body=state;
   }
   return route.fulfill({contentType:'application/json',body:JSON.stringify(body)});
  });
  await page.goto('http://ui.test/');
  await page.evaluate(()=>{switchPanel('im');IM_ACTIVE=1;imRunUpdate({task_id:91,session_id:1,status:'running',stage:'waiting_for_executor',activity:'正在等待可用执行器',agent:'alpha'});});
  assert.match(await page.locator('#im-run-status').innerText(),/正在等待可用执行器/);
  assert.doesNotMatch(await page.locator('#im-run-status').innerText(),/正在讨论/);
  await page.locator('#im-stop').click();
  await page.waitForFunction(()=>IM_RUN_STATE.get(91)?.status==='cancelled');
  assert.equal(await page.locator('#im-stop').isVisible(),false);
  assert.match(await page.locator('#im-run-status').innerText(),/讨论已停止/);
  await page.evaluate(()=>imRunUpdate({task_id:91,session_id:1,status:'queued'}));
  assert.match(await page.locator('#im-run-status').innerText(),/讨论已停止/,'a stale queued event must not revive a cancelled wait');
  assert.deepEqual(stops,[91]);assert.deepEqual(errors,[]);
  console.log('PASS: capacity waits are explicit and cancellable; terminal status survives delayed queued events');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
