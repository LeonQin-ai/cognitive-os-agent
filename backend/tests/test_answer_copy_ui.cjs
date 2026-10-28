const {chromium}=require('playwright');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const http=require('node:http');

(async()=>{
  const html=fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8');
  const server=http.createServer((req,res)=>{res.writeHead(200,{'Content-Type':'text/html; charset=utf-8'});res.end(html);});
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});
  try{
    const page=await browser.newPage({viewport:{width:1100,height:800}}),errors=[];
    page.on('pageerror',e=>errors.push(e.message));
    await page.route('**/v1/**',route=>{
      const u=new URL(route.request().url());
      const data=u.pathname==='/v1/config/llm'?{provider:'openai',model:'fixture',configured:true,api_key_set:true}:
        u.pathname==='/v1/chat/sessions'?(route.request().method()==='POST'?{id:'copy-session'}:[{id:'copy-session',turns:0}]):
        u.pathname==='/v1/chat/history'?{turns:[],runs:[]}:[];
      return route.fulfill({contentType:'application/json',body:JSON.stringify(data)});
    });
    await page.goto(`http://127.0.0.1:${server.address().port}`);
    await page.waitForFunction(()=>CHAT_SESSION==='copy-session');
    await page.evaluate(()=>{
      window.copied=[];
      Object.defineProperty(navigator,'clipboard',{configurable:true,value:{writeText:async text=>{window.copied.push(text);}}});
      CHAT_MSGS=[];$('chat-box').replaceChildren();
      chatAddBubble('bot','');
      window.copyEntry=CHAT_MSGS.at(-1);
      copyEntry.pending=true;
      copyEntry.progress={stage:'planning',seq:1,tool_calls:0};
      renderChatEntry(copyEntry);
    });
    assert.equal(await page.locator('.bot > .chat-copy').count(),0,'no copy action for an empty pending answer');
    const raw='# 完整结果\r\n\r\n```sh\necho "中文 <tag> & more"\n```\n![image](https://example.test/result.png)\n'+ 'long output 0123456789\n'.repeat(2500)+'EOF_COPY_PROOF';
    await page.evaluate(text=>chatFinish(CHAT_SESSION,991230,copyEntry,text,'completed'),raw);
    await page.locator('.bot > .chat-copy').click();
    assert.equal(await page.evaluate(()=>copied.at(-1)),raw,'copy the entire original Markdown, including images and tail');
    assert.equal((await page.locator('.bot > .chat-copy').evaluate(el=>getComputedStyle(el).opacity)),'1','assistant copy is discoverable without hover');
    await page.evaluate(()=>chatRedraw());
    await page.locator('.bot > .chat-copy').click();
    assert.equal(await page.evaluate(()=>copied.at(-1)),raw,'copy survives history redraw');
    await page.evaluate(()=>{copyEntry=CHAT_MSGS.at(-1);copyEntry.text='失败后保留的阶段结论\nPARTIAL_TAIL';copyEntry.progress.stage='failed';renderChatEntry(copyEntry);});
    await page.locator('.bot > .chat-copy').click();
    assert.equal(await page.evaluate(()=>copied.at(-1)),'失败后保留的阶段结论\nPARTIAL_TAIL','partial failure answers are also copyable');
    await page.setViewportSize({width:390,height:844});
    assert.equal(await page.locator('.bot > .chat-copy').isVisible(),true,'mobile copy stays accessible');
    // Exercise a denied/unavailable Clipboard API and legacy copy returning false.
    await page.evaluate(()=>{
      Object.defineProperty(navigator,'clipboard',{configurable:true,value:{writeText:async()=>{throw Error('denied');}}});
      window.fallbackText='';document.execCommand=command=>{if(command==='copy')fallbackText=document.activeElement.value;return true;};
    });
    await page.locator('.bot > .chat-copy').click();
    await page.waitForFunction(()=>fallbackText==='失败后保留的阶段结论\nPARTIAL_TAIL');
    await page.evaluate(()=>{Object.defineProperty(navigator,'clipboard',{configurable:true,value:undefined});document.execCommand=()=>false;});
    await page.locator('.bot > .chat-copy').click();
    await page.waitForFunction(()=>$('toast').textContent==='复制失败');
    assert.equal(await page.locator('body > textarea').count(),0,'fallback removes its temporary textarea');
    assert.deepEqual(errors,[]);
    console.log('PASS #230: completed and partial answers, full Markdown/tail fidelity, redraw, mobile and Clipboard API fallback/failure.');
  }finally{await browser.close();await new Promise(resolve=>server.close(resolve));}
})().catch(error=>{console.error(error);process.exitCode=1;});
