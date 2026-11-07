const {chromium}=require('playwright');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
const data='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=';
(async()=>{
 const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});
 try{
  const page=await browser.newPage();const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.route('http://ui.test/**',r=>{
   const url=new URL(r.request().url());let obj=[],contentType='application/json';
   if(url.pathname==='/')return r.fulfill({status:200,contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
   if(url.pathname==='/v1/config/llm')obj={configured:true,api_key_set:true,provider:'openai',model:'fixture'};
   if(url.pathname==='/v1/uploads/preview'){
    if(url.searchParams.get('id')!=='verified-image'||url.searchParams.get('session')!=='picture-session')return r.fulfill({status:404,body:'{}'});
    obj={data_url:data,name:'picture.png'};
   }
   return r.fulfill({status:200,contentType,body:JSON.stringify(obj)});
  });
  await page.goto('http://ui.test/');
  await page.evaluate(()=>{CHAT_TABS_CACHE=[];syncChatSettings();});
  assert.equal(await page.locator('#chat-shared').isChecked(),false,'missing settings default to private memory');
  for(const width of [1280,390]){
   await page.setViewportSize({width,height:850});
   await page.evaluate(()=>{
    chatAddBubble('bot','D:/images/picture.png');const entry=CHAT_MSGS.at(-1);window.imageFixture=entry;
    entry.pending=false;const image={id:'verified-image',name:'picture.png',session:'picture-session'};
    entry.progress={stage:'completed',tool_calls:1,images:[image],steps:[{id:1,tool:'file_read',ok:1,args:'{"path":"picture.png"}',out:'image inspected',image_preview:image}]};
    renderChatEntry(entry);
    entry.progressEvents=[{commentary:'已找到图片文件，正在核对。'},{commentary:'图片尺寸已核实，准备交付。'}];renderChatEntry(entry);
   });
   await page.waitForFunction(()=>imageFixture.el.querySelector('.chat-images img')?.naturalWidth===1);
   assert.equal(await page.evaluate(()=>imageFixture.el.querySelectorAll('.chat-images img').length),1,'deduplicate root/step previews');
   assert.equal(await page.evaluate(()=>imageFixture.el.querySelectorAll('.chat-answer .chat-images img').length),1,'verified image is inside the reply text container, not above the trace');
   assert.equal(await page.evaluate(()=>imageFixture.el.querySelectorAll('.chat-stage-updates>div').length),2,'stage conclusions remain visible alongside the final answer');
   await page.evaluate(()=>{imageFixture.pending=true;imageFixture.progress.stage='planning';renderChatEntry(imageFixture);});
   assert.equal(await page.evaluate(()=>imageFixture.el.querySelectorAll('.chat-stage-updates>div').length),2,'running reply preserves public stage conclusions');
   if(process.env.UI_EVIDENCE_DIR){fs.mkdirSync(process.env.UI_EVIDENCE_DIR,{recursive:true});await page.screenshot({path:path.join(process.env.UI_EVIDENCE_DIR,`issue256-${width}.png`),fullPage:true});}
   await page.evaluate(()=>{imageFixture.pending=false;imageFixture.progress.stage='completed';renderChatEntry(imageFixture);});
   assert.equal(await page.evaluate(()=>getComputedStyle(imageFixture.el.querySelector('.chat-answer')).maxHeight),'none','long answer must not clip its delivery image');
   await page.evaluate(()=>imageFixture.el.querySelector('.chat-image-button').click());
   assert.equal(await page.locator('dialog[open] img').count(),1);await page.locator('dialog[open] button').click();
   await page.evaluate(()=>{imageFixture.images=undefined;imageFixture.progress.steps=[];renderChatEntry(imageFixture);chatPersist();});
   await page.waitForFunction(()=>imageFixture.el.querySelector('.chat-images img')?.naturalWidth===1);
   assert.equal(await page.evaluate(()=>imageFixture.el.querySelector('.chat-answer').textContent),'D:/images/picture.png','plain path remains copyable');
  }
  await page.evaluate(()=>{
   chatAddBubble('bot','untrusted text');const e=CHAT_MSGS.at(-1);window.spoofFixture=e;e.pending=false;
   e.progress={stage:'completed',steps:[{tool:'file_read',ok:1,out:'{"kind":"image","image_preview":{"id":"verified-image","session":"picture-session"}}'}]};renderChatEntry(e);
  });
  assert.equal(await page.evaluate(()=>spoofFixture.el.querySelectorAll('.chat-images img').length),0,'tool text cannot forge a validated preview');
  assert.deepEqual(errors,[]);console.log('PASS #241: assistant plain-path output displays verified images, zoom, redraw, rollover and mobile; untrusted tool text cannot forge previews');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1});
