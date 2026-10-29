const {chromium}=require('playwright');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const http=require('node:http');
(async()=>{
 const html=fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8');
 const server=http.createServer((req,res)=>{res.writeHead(200,{'Content-Type':'text/html; charset=utf-8'});res.end(html);});
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 const browser=await chromium.launch({headless:true,...(process.env.UI_TEST_EXECUTABLE?{executablePath:process.env.UI_TEST_EXECUTABLE}:{channel:process.env.UI_TEST_BROWSER||'msedge'})});
 try{
  const page=await browser.newPage({timezoneId:'Asia/Shanghai',locale:'zh-CN',viewport:{width:390,height:844}}),errors=[];
  page.on('pageerror',error=>errors.push(error.message));
  await page.route('**/v1/**',route=>{
   const u=new URL(route.request().url());
   const value=u.pathname==='/v1/config/llm'?{provider:'openai',model:'fixture',configured:true,api_key_set:true}:
    u.pathname==='/v1/chat/sessions'?(route.request().method()==='POST'?{id:'timestamp-session'}:[{id:'timestamp-session',turns:0}]):
    u.pathname==='/v1/chat/history'?{turns:[],runs:[]}:[];
   return route.fulfill({contentType:'application/json',body:JSON.stringify(value)});
  });
  await page.goto(`http://127.0.0.1:${server.address().port}`);
  await page.waitForFunction(()=>CHAT_SESSION==='timestamp-session');
  const start=Date.UTC(2026,9,29,1,2,3),finish=start+5000;
  await page.evaluate(({start,finish})=>{
   CHAT_MSGS=chatHistoryEntries([{q:'日期测试',a:'完成答复'}],[{id:234,input:'日期测试',output:'完成答复',status:'DONE',ts:finish,trace:[{started_ms:start,elapsed_ms:5000,stage:'completed'}]}]);
   CHAT_CACHE[CHAT_SESSION]=CHAT_MSGS;chatRedraw();chatPersist();
  },{start,finish});
  assert.deepEqual(await page.locator('.chat-date-time').evaluateAll(nodes=>nodes.map(n=>n.dateTime)),[new Date(start).toISOString(),new Date(finish).toISOString()]);
  assert.match(await page.locator('.user .chat-date-time').innerText(),/2026.*10.*29.*09:02:03/);
  assert.equal(await page.locator('.bot .chat-date-time').getAttribute('title'),'Asia/Shanghai');
  await page.evaluate(()=>{CHAT_MSGS=[];delete CHAT_CACHE[CHAT_SESSION];chatRestore();CHAT_MSGS=CHAT_CACHE[CHAT_SESSION];chatRedraw();});
  assert.deepEqual(await page.locator('.chat-date-time').evaluateAll(nodes=>nodes.map(n=>n.dateTime)),[new Date(start).toISOString(),new Date(finish).toISOString()],'cached history restores exact instants');
  await page.evaluate(()=>{CHAT_MSGS=chatHistoryEntries([{q:'旧记录',a:'旧答复'}],[]);chatRedraw();});
  assert.deepEqual(await page.locator('.chat-date-time').allTextContents(),['时间未记录','时间未记录'],'unknown historical times are not replaced with current time');
  await page.evaluate(()=>{CHAT_MSGS=[];$('chat-box').replaceChildren();chatAddBubble('user','新消息');chatAddBubble('bot','');window.timestampEntry=CHAT_MSGS.at(-1);timestampEntry.pending=true;timestampEntry.progress={stage:'planning',seq:1};renderChatEntry(timestampEntry);});
  assert.equal(await page.locator('.chat-date-time').count(),2);
  assert.ok(await page.locator('.user .chat-date-time').getAttribute('datetime'));
  await page.evaluate(()=>chatFinish(CHAT_SESSION,234,timestampEntry,'新答复','completed'));
  await page.evaluate(()=>chatRedraw());
  assert.equal(await page.locator('.bot .chat-date-time').count(),1,'completion and redraw do not duplicate timestamps');
  assert.equal(await page.locator('.bot > .chat-copy').count(),1,'timestamp preserves complete answer copy');
  assert.deepEqual(errors,[]);
  console.log('PASS #234: local dates, journal restoration, cache persistence, missing historical dates, live completion and mobile redraw');
 }finally{await browser.close();await new Promise(resolve=>server.close(resolve));}
})().catch(error=>{console.error(error);process.exit(1);});
