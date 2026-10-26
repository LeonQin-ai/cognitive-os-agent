const {chromium}=require('playwright');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
(async()=>{
 const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});
 try{
  const page=await browser.newPage(),errors=[],approvals=[];let dictations=0;page.on('pageerror',e=>errors.push(e.message));
  await page.addInitScript(()=>{class Recognition{start(){window.rec=this;this.onstart?.();}stop(){this.onend?.();}}window.SpeechRecognition=Recognition;});
  await page.route('http://ui.test/**',async route=>{
   const req=route.request(),u=new URL(req.url());let obj=[];
   if(u.pathname==='/')return route.fulfill({contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
   if(u.pathname==='/v1/workspaces')obj={default_workspace:'D:/default',workspaces:[]};
   if(u.pathname==='/v1/workspaces/browse')obj={path:'D:/项目 folder'};
   if(u.pathname==='/v1/voice/dictate'){dictations++;obj={text:'系统语音'};}
   if(u.pathname==='/v1/config/llm')obj={provider:'openai',model:'fixture',configured:true,api_key_set:true};
   if(u.pathname==='/v1/chat/sessions'&&req.method()==='POST')obj={id:'s',workspace:'D:/default'};
   if(u.pathname==='/v1/tasks/7/approval'){approvals.push(req.postDataJSON());obj={ok:true};}
   return route.fulfill({contentType:'application/json',body:JSON.stringify(obj)});
  });
  await page.goto('http://ui.test/');await page.locator('#chat-input').waitFor();assert.equal(await page.locator('#chat-mic').count(),0,'unavailable dictation entry is removed (#217)');
  await page.locator('#chat-permission').selectOption('ask');await page.evaluate(()=>loadChat());
  assert.equal(await page.locator('#chat-permission').inputValue(),'ask');
  await page.locator('#chat-thinking-toggle').click();
  assert.match(await page.locator('#chat-thinking-toggle').innerText(),/已开启/);
  assert.notEqual(await page.locator('#chat-thinking-toggle').evaluate(e=>getComputedStyle(e).color),'rgb(255, 255, 255)');
  await page.locator('#chat-thinking-toggle').click();
  assert.equal(await page.locator('#chat-thinking-toggle').getAttribute('aria-pressed'),'false');
  await page.evaluate(()=>workspaceDialog());await page.locator('#workspace-browse').click();
  await page.waitForFunction(()=>$('workspace-path').value==='D:/项目 folder');
  await page.locator('#workspace-dialog button').filter({hasText:'取消'}).click();
  await page.evaluate(()=>chatApproval({id:7,approval:{id:3,tool:'shell',args:'<script>never execute</script>'}},'s'));
  assert.equal(await page.evaluate(()=>{const b=$('chat-approval').querySelector('button');chatApproval({id:7,approval:{id:3,tool:'shell',args:'<script>never execute</script>'}},'s');return b===$('chat-approval').querySelector('button');}),true,'polling must not replace a focused approval button');
  assert.match(await page.locator('#chat-approval').innerText(),/<script>never execute/);
  await page.locator('#chat-approval').getByRole('button',{name:'批准本次'}).click();
  await page.waitForFunction(()=>!!document.querySelector('#chat-approval button:disabled'));
  assert.deepEqual(approvals,[{id:3,session:'s',allow:true}]);
  await page.evaluate(()=>{switchPanel('agents');flowLiveProgress({id:42,status:'RUNNING',flow_nodes:[{id:'step1',agent:'architect',status:'running',activity:'正在读取代码',elapsed_ms:1234,llm_calls:2,tool_calls:1,tokens_in:100,tokens_out:20,steps:[{tool:'shell',args:'echo evidence',out:'verified',ok:1,ms:12}]},{id:'step2',agent:'reviewer',status:'queued'}]});});
  const progress=await page.locator('#flow-live').innerText();assert.match(progress,/architect/);assert.match(progress,/正在读取代码/);assert.match(progress,/120/);assert.match(progress,/等待依赖/);assert.match(progress,/12ms/);
  await page.locator('#flow-live details details summary').click();assert.match(await page.locator('#flow-live').innerText(),/verified/);
  await page.evaluate(()=>flowLiveProgress({id:42,status:'FAILED',flow_nodes:[{id:'step1',agent:'architect',status:'error',model_error:'provider failed'}]}));
  assert.match(await page.locator('#flow-live').innerText(),/provider failed/);
  await page.setViewportSize({width:390,height:844});await page.evaluate(()=>switchPanel('chat'));assert.equal(await page.locator('#chat-permission').isVisible(),true);
  assert.deepEqual(errors,[]);console.log('PASS: native folder choice, persistent permission control, unavailable microphone entry removal, escaped one-shot approval, live Flow metrics/tools/errors and mobile access');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
