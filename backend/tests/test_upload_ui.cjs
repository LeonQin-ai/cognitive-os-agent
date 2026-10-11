const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
(async()=>{
  const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER || 'msedge'});
  try {
    const page=await browser.newPage({viewport:{width:1280,height:800}});
    let release, gate=null, uploaded=0;
    const posts=[], errors=[];
    page.on('pageerror',e=>errors.push(e.message));
    await page.route('**/v1/**',async route=>{
      const url=new URL(route.request().url());
      let obj=[];
      if(url.pathname==='/v1/upload'){
        if(gate) await gate;
        obj={ok:true,id:String(++uploaded).padStart(32,'0'),name:url.searchParams.get('name'),status:'ready'};
      } else if(url.pathname==='/v1/chat' && route.request().method()==='POST'){
        posts.push(route.request().postDataJSON()); obj={id:9000+posts.length};
      } else if(url.pathname.startsWith('/v1/tasks/')) obj={id:9000+posts.length,status:'DONE',output:'完成'};
      else if(url.pathname.includes('/config/llm')) obj={provider:'openai',model:'test',configured:true,api_key_set:true};
      else if(url.pathname.includes('/chat/history')) obj={turns:[],runs:[]};
      await route.fulfill({status:200,contentType:'application/json',body:JSON.stringify(obj)});
    });
    await page.goto(process.env.UI_TEST_URL || 'http://127.0.0.1:18771');
    await page.waitForTimeout(250);
    await page.evaluate(async()=>{
      await chatSwitchSession('A');
      await uploadBlob('A.txt',new Blob(['A file']));
      await chatSwitchSession('B');
    });
    assert.equal(await page.evaluate(()=>CHAT_ATTACH.length),0,'A attachments must not appear in B');
    await page.evaluate(()=>chatSwitchSession('A'));
    assert.equal(await page.evaluate(()=>CHAT_ATTACH[0].name),'A.txt');
    await page.evaluate(()=>{chatAttachRemove(0);});
    gate=new Promise(resolve=>{release=resolve;});
    await page.evaluate(()=>{
      window.uploadJob=uploadBlob('slow.txt',new Blob(['slow upload']));
      $('chat-input').disabled=false; $('chat-input').value='总结附件';
      window.sendJob=chatSubmit();
    });
    await page.waitForTimeout(150);
    assert.equal(posts.length,0,'chat waits for pending upload');
    await page.evaluate(()=>chatSwitchSession('B'));
    release(); gate=null;
    await page.evaluate(()=>window.uploadJob);
    await page.evaluate(()=>window.sendJob);
    assert.equal(posts.length,1);
    assert.equal(posts[0].session,'A');
    assert.equal(posts[0].attachments.length,1);
    assert.equal(await page.evaluate(()=>CHAT_ATTACH.length),0,'late A upload must not appear in B');

    for(const viewport of [{width:1280,height:800},{width:390,height:844}]){
      await page.setViewportSize(viewport);
      await page.evaluate(()=>{
        $('chat-box').innerHTML=''; CHAT_MSGS=[]; CHAT_FOLLOW=true;
        for(let i=0;i<20;i++) chatAddBubble('user','较长的历史消息 '.repeat(20));
        const el=chatAddBubble('bot','');
        const entry=CHAT_MSGS[CHAT_MSGS.length-1];
        entry.pending=true; entry.taskId=777;
        entry.progress={stage:'planning',seq:1,model_call:1,started_ms:Date.now()};
        CHAT_ENTRIES.set(777,{session:CHAT_SESSION,entry});
        window.scrollEntry=entry; renderChatEntry(entry); chatScroll(true);
      });
      await page.waitForTimeout(100);
      assert.equal(await page.evaluate(()=>chatNearBottom()),true);
      await page.evaluate(()=>chatApplyProgress(CHAT_SESSION,777,{seq:2,stage:'executing',cur_tool:'file_read',activity:'正在分析附件内容 '.repeat(100)}));
      await page.waitForTimeout(100);
      assert.equal(await page.evaluate(()=>chatNearBottom()),true,'progress follows bottom');
      const artifacts=path.resolve(__dirname,'../build/ui-progress-qa'); fs.mkdirSync(artifacts,{recursive:true});
      await page.screenshot({path:path.join(artifacts,`thinking-follow-${viewport.width}.png`)});
      await page.evaluate(()=>{
        const box=$('chat-box'); box.scrollTop=0; box.dispatchEvent(new Event('scroll'));
      });
      await page.waitForTimeout(100);
      const top=await page.evaluate(()=>$('chat-box').scrollTop);
      await page.evaluate(()=>chatApplyProgress(CHAT_SESSION,777,{seq:3,stage:'recovering',activity:'正在整理结果 '.repeat(100)}));
      await page.waitForTimeout(100);
      assert.equal(await page.evaluate(()=>$('chat-box').scrollTop),top,'manual history reading remains in place');
      await page.evaluate(()=>{
        chatScroll(true);
        chatAddBubble('user','新的大段消息 '.repeat(300));
      });
      await page.waitForTimeout(100);
      assert.equal(await page.evaluate(()=>chatNearBottom()),true,'long new message follows after layout');
      await page.screenshot({path:path.join(artifacts,`scroll-${viewport.width}.png`)});
    }
    assert.deepEqual(errors,[]);
    console.log('PASS: attachment session isolation, upload-before-send, switching during upload, desktop/mobile auto-scroll and manual history position.');
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
