const {chromium}=require('playwright'),fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
(async()=>{const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});try{
 const page=await browser.newPage(),records=[],submitted=[],errors=[];page.on('pageerror',e=>errors.push(e.message));
 await page.route('http://ui.test/**',async route=>{const r=route.request(),u=new URL(r.url());let obj=[];
  if(u.pathname==='/')return route.fulfill({contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
  if(u.pathname==='/v1/workspaces')obj={default_workspace:'D:/default',workspaces:[{path:'D:/selected',name:'Selected',created_ms:2}]};
  if(u.pathname==='/v1/chat/sessions')obj=r.method()==='POST'?{id:'s',workspace:'D:/default'}:[{id:'s',title:'Failed session',last_run:{id:42,status:'FAILED'},workspace:'D:/default'}];
  if(u.pathname==='/v1/config/llm')obj={provider:'openai',model:'fixture',api_key_set:true};
  if(u.pathname==='/v1/chat/history')obj={turns:[],runs:[{id:42,status:'FAILED',input:'failed request',output:'failure evidence',trace:[{stage:'failed',elapsed_ms:880000,steps:[{tool:'file_read',ok:0,target:'missing'}]}]}]};
  if(u.pathname==='/v1/flows/list')obj={records:[...records].reverse()};
  if(['/v1/orchestrate','/v1/flows'].includes(u.pathname)&&r.method()==='POST'){
   const body=r.postDataJSON(),id=100+records.length;submitted.push({path:u.pathname,body});records.push({id,task_id:id,status:'DONE',workspace:body.workspace,input:body.task||body.input,automatic:u.pathname==='/v1/orchestrate',created_ms:1,updated_ms:1});obj={id};
  }
  if(u.pathname.startsWith('/v1/tasks/'))obj={id:Number(u.pathname.split('/').pop()),status:'DONE',output:'verified',flow_nodes:[]};
  return route.fulfill({contentType:'application/json',body:JSON.stringify(obj)});
 });
 await page.goto('http://ui.test/');await page.waitForFunction(()=>CHAT_SESSION==='s');
 await page.evaluate(()=>{for(const id of Object.keys(CHAT_CACHE))delete CHAT_CACHE[id];sideRecent([{id:'s',title:'Failed session',workspace:'D:/default',last_run:{id:42,status:'FAILED'}}]);});
 assert.equal(await page.locator('.recent-task-status').innerText(),'失败','cold sidebar restores durable status without browser cache');
 await page.evaluate(()=>{CHAT_CACHE.s=[{role:'bot',taskId:42,progress:{stage:'failed'}}];sideRecent([{id:'s',workspace:'D:/default',last_run:{id:42,status:'DONE'}}]);});
 assert.equal(await page.locator('.recent-task-status').innerText(),'已完成','server success overrides an obsolete cached failure');
 const history=await page.evaluate(()=>chatHistoryEntries([], [{id:42,status:'FAILED',input:'failed request',output:'failure evidence',trace:[{stage:'failed',elapsed_ms:880000,steps:[{tool:'file_read',ok:0,target:'missing'}]}]}]));
 assert.equal(history[1].progress.stage,'failed');assert.equal(history[1].progress.elapsed_ms,880000);assert.equal(history[1].progress.steps[0].ok,0);
 await page.evaluate(()=>switchPanel('agents'));await page.locator('#flow-workspace option[value="D:/selected"]').waitFor({state:'attached'});
 await page.locator('#flow-workspace').selectOption('D:/selected');await page.locator('#flow-task').fill('Read and summarize marker');
 await page.getByRole('button',{name:'自动执行',exact:true}).click();await page.waitForFunction(()=>!FLOW_BUSY);
 await page.locator('#flow-task-100').waitFor();assert.match(await page.locator('#flow-task-100').innerText(),/D:\/selected/);
 await page.locator('#flow-json').fill(JSON.stringify({nodes:[{id:'a',agent:'alpha',task:'read marker'}]}));
 await page.getByRole('button',{name:'执行 Flow',exact:true}).click();await page.waitForFunction(()=>!FLOW_BUSY);await page.locator('#flow-task-101').waitFor();
 assert.deepEqual(submitted.map(x=>x.body.workspace),['D:/selected','D:/selected']);assert.ok(submitted[1].body.flow.nodes);
 await page.evaluate(()=>switchPanel('chat'));await page.evaluate(()=>switchPanel('agents'));
 await page.waitForFunction(()=>$('flow-workspace').value==='D:/selected');assert.equal(await page.locator('#flow-task-100').count(),1);
 assert.deepEqual(errors,[]);console.log('PASS: auto/manual selected workspace, immediate recent records, panel return and failed status/history without browser storage');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
