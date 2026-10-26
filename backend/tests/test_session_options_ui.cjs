const {chromium}=require('playwright'),fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
(async()=>{const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});try{
 const page=await browser.newPage(),sessions=[],submitted=[];let globalWrites=0;await page.route('http://ui.test/**',async route=>{
 const r=route.request(),u=new URL(r.url());let obj=[];
 if(u.pathname==='/')return route.fulfill({contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
 if(u.pathname==='/v1/config/llm'){if(r.method()==='POST')globalWrites++;obj={provider:'openai',model:'global',api_key_set:true};}
 if(u.pathname==='/v1/routes')obj=[{name:'route-a',provider:'openai',model:'a'},{name:'route-b',provider:'openai',model:'b'}];
 if(u.pathname==='/v1/workspaces')obj={default_workspace:'D:/default',workspaces:[]};
 if(u.pathname==='/v1/chat/sessions'){if(r.method()==='POST'){obj={id:sessions.length?'b':'a',workspace:'D:/default'};sessions.push(obj);}else obj=sessions;}
 if(u.pathname==='/v1/chat/history')obj={turns:[],runs:[]};
 if(u.pathname==='/v1/chat'){submitted.push(r.postDataJSON());obj={id:7};}
 if(u.pathname==='/v1/tasks/7')obj={id:7,status:'DONE',output:'verified'};
 return route.fulfill({contentType:'application/json',body:JSON.stringify(obj)});});
 await page.goto('http://ui.test/');await page.waitForFunction(()=>CHAT_SESSION==='a' && $('chat-model').options.length===3);
 await page.locator('#chat-model').selectOption('route-a');await page.locator('#chat-permission').selectOption('ask');await page.locator('#chat-thinking-toggle').click();
 await page.locator('#nav-newchat').click();await page.waitForFunction(()=>CHAT_SESSION==='b' && $('chat-model').value==='@cur');
 assert.equal(await page.locator('#chat-permission').inputValue(),'auto');assert.equal(await page.locator('#chat-thinking-toggle').getAttribute('aria-pressed'),'false');
 await page.locator('#chat-model').selectOption('route-b');await page.locator('#chat-permission').selectOption('full');
 await page.evaluate(()=>chatSwitchSession('a'));await page.waitForFunction(()=>$('chat-model').value==='route-a');
 assert.equal(await page.locator('#chat-permission').inputValue(),'ask');assert.equal(await page.locator('#chat-thinking-toggle').getAttribute('aria-pressed'),'true');
 await page.locator('#chat-input').fill('message for A');await page.evaluate(()=>{const upload=new Promise(r=>window.finishUpload=r);CHAT_ATTACH.push({id:'fixture-image',name:'fixture.png',kind:'image',status:'ready',upload});window.submission=chatSubmit();});
 await page.waitForFunction(()=>CHAT_SUBMITTING.has('a'));await page.evaluate(()=>chatSwitchSession('b'));await page.locator('#chat-input').fill('draft for B');
 await page.evaluate(()=>finishUpload());await page.waitForFunction(()=>!CHAT_SUBMITTING.has('a'));await page.evaluate(()=>submission);
 assert.equal(submitted[0].session,'a');assert.equal(submitted[0].route,'route-a');assert.equal(submitted[0].permission_mode,'ask');assert.equal(submitted[0].thinking,true);
 assert.equal(await page.locator('#chat-input').inputValue(),'draft for B','waiting submission must not clear another chat draft');
 assert.equal(await page.evaluate(()=>CHAT_MSGS.some(m=>m.text==='message for A')),false,'pending attachment submission belongs only to original conversation');
 assert.equal(globalWrites,0);console.log('PASS #222: per-chat model/access/thinking, fresh defaults, switching restore and immutable settings while attachment upload waits');
}finally{await browser.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
