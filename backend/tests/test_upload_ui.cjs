const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const path = require('node:path');
const fs = require('node:fs');
(async()=>{
  const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER || 'msedge'});
  try {
    const page=await browser.newPage({viewport:{width:1280,height:800}});
    let release, gate=null, configGate=null, sessionCreateGate=null, uploaded=0, sessionCreates=0, lastCreatedSession=null, failSessionCreates=false;
    const staleTaskId=77881; let loseStaleTask=false, staleTaskPolls=0;
    const posts=[], routeWrites=[], errors=[], sessionDeletes=[];
    let routeList=[];
    let sessions=[{id:'default',title:'默认会话',turns:2}];
    page.on('pageerror',e=>errors.push(e.message));
    await page.route('**/v1/**',async route=>{
      const url=new URL(route.request().url());
      let obj=[];
      if(url.pathname==='/v1/upload'){
        if(gate) await gate;
        const name=url.searchParams.get('name');
        obj={ok:true,id:String(++uploaded).padStart(32,'0'),name,status:'ready',kind:/\.(png|jpe?g|webp|gif)$/i.test(name)?'image':'text'};
      } else if(url.pathname==='/v1/uploads/preview'){
        obj={data_url:'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aK1sAAAAASUVORK5CYII='};
      } else if(url.pathname==='/v1/routes' && route.request().method()==='GET'){
        obj=routeList;
      } else if(url.pathname==='/v1/chat/sessions' && route.request().method()==='GET'){
        obj=sessions;
      } else if(url.pathname==='/v1/chat/sessions' && route.request().method()==='POST'){
        sessionCreates++;
        if(failSessionCreates){
          await route.fulfill({status:500,contentType:'application/json',body:JSON.stringify({error:'fixture create failure'})});
          return;
        }
        const id=`new-session-${sessionCreates}`;
        if(sessionCreateGate) await sessionCreateGate;
        lastCreatedSession=id;
        obj={id}; sessions=[...sessions,{id,turns:0}];
      } else if(url.pathname==='/v1/chat/sessions/default/delete' && route.request().method()==='POST'){
        sessionDeletes.push('default'); sessions=sessions.filter(s=>s.id!=='default'); obj={status:'deleted'};
      } else if(url.pathname==='/v1/routes' && route.request().method()==='POST'){
        routeWrites.push(route.request().postDataJSON()); obj={ok:true};
      } else if(url.pathname==='/v1/chat' && route.request().method()==='POST'){
        posts.push(route.request().postDataJSON()); obj={id:9000+posts.length};
      } else if(url.pathname===`/v1/tasks/${staleTaskId}` && route.request().method()==='GET'){
        staleTaskPolls++;
        if(loseStaleTask){ await route.fulfill({status:404,contentType:'application/json',body:JSON.stringify({error:'task not found'})}); return; }
        obj={id:staleTaskId,status:'RUNNING',stage:'executing',session_id:'old-task-session'};
      } else if(url.pathname.startsWith('/v1/tasks/')) obj={id:9000+posts.length,status:'DONE',output:'完成'};
      else if(url.pathname.includes('/config/llm')){
        if(configGate) await configGate;
        obj={provider:'openai',model:'test',configured:true,api_key_set:true};
      }
      else if(url.pathname.includes('/chat/history')){
        const session=new URLSearchParams(url.search).get('session');
        obj=session==='old-task-session'
          ? {turns:[{q:'旧任务输入',a:'旧任务已成功完成'}],runs:[
              {id:staleTaskId,status:'RUNNING',input:'旧任务输入',output:'',ts:1},
              {id:staleTaskId,status:'DONE',input:'旧任务输入',output:'旧任务已成功完成',ts:2}
            ]}
          : {turns:[],runs:[]};
      }
      await route.fulfill({status:200,contentType:'application/json',body:JSON.stringify(obj)});
    });
    await page.goto(process.env.UI_TEST_URL || 'http://127.0.0.1:18771');
    await page.waitForTimeout(250);
    routeList=[{name:'ark-deepseek-v4.1-flash',provider:'openai',model:'deepseek-v4.1-flash',base_url:'https://ark.invalid/api/v3',context_window:1074176}];
    await page.evaluate(()=>switchPanel('models'));
    await page.waitForFunction(()=>document.querySelector('#routes-body')?.innerText.includes('1074176'));
    assert.match(await page.locator('#routes-body').innerText(),/1074176/,'route table displays the model metadata context window');
    assert.equal(await page.locator('#routes-body button').filter({hasText:'改窗口'}).count(),0,
      'route context window is read-only and cannot be manually changed');
    routeList=[];
    await page.evaluate(()=>{
      CHAT_SESSION='default';
      renderChatTabs([{id:'default',title:'默认会话',turns:2}]);
    });
    const defaultRow=page.locator('#side-recent .side-recent-row').filter({hasText:'默认会话'});
    assert.equal(await defaultRow.locator('.side-recent-delete').count(),1,
      'default session exposes delete only, no clear button');
    await page.evaluate(()=>{
      window.confirm=()=>true;
      CHAT_SESSION='default'; CHAT_MSGS=[{role:'user',text:'要删除的默认会话内容'}];
      CHAT_CACHE.default=CHAT_MSGS;
    });
    await page.evaluate(()=>chatDeleteSession('default'));
    assert.deepEqual(sessionDeletes,['default'],'default session uses the delete endpoint');
    assert.equal(await page.evaluate(()=>CHAT_SESSION),lastCreatedSession,'deleting the last/default session opens a fresh session');
    assert.equal(await page.evaluate(()=>Object.hasOwn(CHAT_CACHE,'default')),false,'deleted default transcript is removed from UI cache');
    await page.waitForFunction(()=>![...document.querySelectorAll('#side-recent .recent-session-title')].some(el=>el.textContent==='默认会话'));
    assert.equal(await page.locator('#side-recent .recent-session-title').filter({hasText:'默认会话'}).count(),0,
      'deleted default is not reintroduced in recent sessions');
    await page.evaluate(()=>{ CHAT_SESSION='default'; CHAT_NEW_SESSION=false; switchPanel('models'); });
    const createsBeforeNav=sessionCreates;
    const navSessionId=`new-session-${createsBeforeNav+1}`;
    await page.locator('#nav-newchat').click();
    await page.waitForFunction(expected=>CHAT_SESSION===expected,navSessionId);
    assert.equal(sessionCreates,createsBeforeNav+1,'new-chat click creates one durable session despite triggering a panel load');
    assert.equal(await page.evaluate(()=>current),'chat','new-chat click opens the chat panel');
    const firstCreatedSession=await page.evaluate(()=>CHAT_SESSION);
    const createsBeforeSecondClick=sessionCreates;
    const secondNavSessionId=`new-session-${createsBeforeSecondClick+1}`;
    await page.locator('#nav-newchat').click();
    await page.waitForFunction(expected=>CHAT_SESSION===expected,secondNavSessionId);
    assert.equal(sessionCreates,createsBeforeSecondClick+1,'clicking new-chat again from the chat panel creates exactly one more durable session');
    assert.notEqual(await page.evaluate(()=>CHAT_SESSION),firstCreatedSession,'each new-chat click selects its newly created session');

    // Real providers can take time to load configuration after the session ID
    // arrives. A second actual click must not join that previous creation.
    await page.waitForFunction(()=>CHAT_NEW_SESSION_REQUEST===null);
    let releaseConfig;
    configGate=new Promise(resolve=>{releaseConfig=resolve;});
    const beforeSlowConfig=sessionCreates;
    await page.locator('#nav-newchat').click();
    await page.waitForFunction(expected=>CHAT_SESSION===expected,`new-session-${beforeSlowConfig+1}`);
    await page.waitForFunction(()=>CHAT_NEW_SESSION_REQUEST===null);
    await page.locator('#nav-newchat').click();
    await page.waitForFunction(expected=>CHAT_SESSION===expected,`new-session-${beforeSlowConfig+2}`);
    await page.waitForFunction(()=>CHAT_NEW_SESSION_REQUEST===null);
    assert.equal(sessionCreates,beforeSlowConfig+2,'two explicit clicks create two independent sessions while configuration is delayed');
    assert.equal(await page.locator('#chat-box .chat-message').count(),0,'the second new chat has an empty conversation');
    configGate=null; releaseConfig();

    // Navigate from another panel with an existing conversation selected, and
    // click again while the creation POST itself is still pending.
    await page.evaluate(()=>switchPanel('models'));
    let releaseCreate;
    sessionCreateGate=new Promise(resolve=>{releaseCreate=resolve;});
    const beforeSlowCreate=sessionCreates;
    await page.locator('#nav-newchat').click();
    await page.locator('#nav-newchat').click();
    sessionCreateGate=null; releaseCreate();
    await page.waitForFunction(expected=>CHAT_SESSION===expected,`new-session-${beforeSlowCreate+2}`);
    await page.waitForFunction(()=>CHAT_NEW_SESSION_REQUEST===null);
    assert.equal(sessionCreates,beforeSlowCreate+2,'explicit clicks queue distinct sessions while the create POST is delayed');
    assert.equal(await page.evaluate(()=>current),'chat','the new session remains in the chat panel after late panel loads');

    const currentAfterCreate=await page.evaluate(()=>CHAT_SESSION);
    const createsBeforeFailure=sessionCreates;
    failSessionCreates=true;
    await page.locator('#nav-newchat').click();
    await page.waitForFunction(()=>document.querySelector('#chat-status')?.textContent.includes('fixture create failure'));
    failSessionCreates=false;
    assert.equal(await page.evaluate(()=>CHAT_SESSION),currentAfterCreate,'failed session creation preserves the current conversation');
    assert.equal(sessionCreates,createsBeforeFailure+1,'failed new-chat click makes one create request and does not silently switch sessions');
    assert.match(await page.locator('#chat-status').textContent(),/新建聊天失败/,'session creation errors are visible');

    /* The journal appends RUNNING and then DONE for the same task. Clicking
       New Chat while the old task is polling must reconcile the latest record
       instead of letting the first RUNNING entry mask its successful result. */
    sessions=[...sessions,{id:'old-task-session',title:'旧会话',turns:1}];
    await page.evaluate(taskId=>{
      const session='old-task-session';
      const entry={role:'bot',text:'',pending:true,taskId,progress:{stage:'executing'}};
      const messages=[{role:'user',text:'旧任务输入',pending:false},entry];
      CHAT_SESSION=session; CHAT_MSGS=messages; CHAT_CACHE[session]=messages;
      CHAT_TASKS[session]=taskId; CHAT_ENTRIES.set(taskId,{session,entry});
      renderChatTabs([...CHAT_TABS_CACHE,{id:session,title:'旧会话',turns:1}]);
      void chatPollTask(session,taskId,entry);
    },staleTaskId);
    await page.waitForTimeout(50);
    assert.ok(staleTaskPolls>0,'old session task poll is active before clicking New Chat');
    loseStaleTask=true;
    const beforeOldTaskNewChat=sessionCreates;
    await page.locator('#nav-newchat').click();
    await page.waitForFunction(expected=>CHAT_SESSION===expected,`new-session-${beforeOldTaskNewChat+1}`);
    await page.waitForFunction(()=>CHAT_CACHE['old-task-session']?.[1]?.progress?.stage==='completed');
    const oldTaskRow=page.locator('#side-recent .side-recent-row').filter({hasText:'旧会话'});
    assert.equal(await oldTaskRow.locator('.recent-task-status').textContent(),'已完成',
      'old task is shown as completed after new-chat click when its latest journal state is DONE');
    assert.equal(await page.evaluate(()=>CHAT_CACHE['old-task-session'][1].progress.stage),'completed');
    await page.evaluate(()=>{
      const entry=CHAT_CACHE['old-task-session'][1];
      entry.progress={...entry.progress,stage:'failed'}; entry.text='旧版本缓存的失败状态';
      sideRecent(CHAT_TABS_CACHE);
    });
    assert.equal(await oldTaskRow.locator('.recent-task-status').textContent(),'失败',
      'the fixture represents an old false-failure state retained in sessionStorage');
    const beforeRefreshNewChat=sessionCreates;
    await page.locator('#nav-newchat').click();
    await page.waitForFunction(expected=>CHAT_SESSION===expected,`new-session-${beforeRefreshNewChat+1}`);
    await page.waitForFunction(()=>CHAT_CACHE['old-task-session']?.[1]?.progress?.stage==='completed');
    assert.equal(await oldTaskRow.locator('.recent-task-status').textContent(),'已完成',
      'new-chat refresh reconciles a persisted false failure with the latest successful journal state');
    const historyRecovery=await page.evaluate(taskId=>chatHistoryEntries([], [
      {id:taskId,status:'RUNNING',input:'历史中的任务',output:'',ts:2}
    ]),staleTaskId);
    assert.equal(historyRecovery[1].progress.stage,'interrupted','stale RUNNING history is not reconstructed as a failed task');
    assert.match(historyRecovery[1].text,/运行状态已丢失.*恢复/,'recovered history explains the task state and recovery action');
    const failedHistory=await page.evaluate(taskId=>chatHistoryEntries([], [
      {id:taskId,status:'FAILED',input:'真实失败任务',output:'执行失败',ts:3}
    ]),staleTaskId);
    assert.equal(failedHistory[1].progress.stage,'failed','a genuine FAILED journal record remains a failure');
    loseStaleTask=false;
    assert.match(await page.locator('#chat-file').getAttribute('accept'),/image\/\*/,'file picker explicitly accepts images');
    await page.evaluate(()=>{
      if(!document.getElementById('r-provider')){
        const provider=document.createElement('select'); provider.id='r-provider';
        provider.innerHTML='<option value="openai" selected>OpenAI</option>'; document.body.appendChild(provider);
      }
      for(const [id,value] of [['fetched-models',''],['r-base','https://provider.invalid/v1'],['r-key','test-key'],['r-model',''],['models-stat','']]){
        if(!document.getElementById(id)){ const el=document.createElement(id==='fetched-models'?'div':'input'); el.id=id; document.body.appendChild(el); }
        const el=document.getElementById(id); if('value' in el) el.value=value;
      }
      ROUTES=[]; FETCHED_MODELS=[{id:'fixture/large',context_window:524288},{id:'fixture/default'}]; renderFetchedModels();
    });
    assert.equal(await page.locator('#fetched-models button').count(),2,'provider model metadata renders as selectable models');
    assert.match(await page.locator('#fetched-models button').first().getAttribute('title'),/524,288/);
    await page.evaluate(()=>pickFetchedModel(0));
    assert.equal(routeWrites.at(-1).model,'fixture/large');
    assert.equal(routeWrites.at(-1).context_window,524288,'provider context_window metadata is persisted on the route');
    assert.equal(await page.locator('#r-context').count(),0,'context capacity is not manually editable');
    await page.evaluate(()=>{
      // Model-form fixtures are body flex children; remove them before visual
      // chat checks so they cannot squeeze main to zero width.
      for(const id of ['r-provider','fetched-models','r-base','r-key','r-model','models-stat']){
        const el=document.getElementById(id); if(el?.parentElement===document.body) el.remove();
      }
    });
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

    const png=Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aK1sAAAAASUVORK5CYII=','base64');
    await page.locator('#chat-input').fill('请描述这张图片');
    await page.locator('#chat-file').setInputFiles({name:'chat-image.png',mimeType:'image/png',buffer:png});
    await page.waitForFunction(()=>CHAT_ATTACH.some(a=>a.name==='chat-image.png' && a.status==='ready' && a.kind==='image'));
    await page.locator('#chat-attach img[alt="chat-image.png"]').waitFor();
    await page.waitForFunction(()=>document.querySelector('#chat-attach img')?.naturalWidth>0);
    await page.evaluate(()=>chatSubmit());
    assert.equal(posts.length,2,'image chat is submitted after upload finishes');
    assert.equal(posts[1].message,'[附件: chat-image.png]\n请描述这张图片');
    assert.equal(posts[1].attachments.length,1,'selected image ID is sent with the chat request');
    await page.locator('#chat-box .user img[alt="chat-image.png"]').waitFor();
    await page.locator('#chat-box .user img[alt="chat-image.png"]').click();
    assert.equal(await page.locator('dialog[open] img').count(),1,'sent image opens a large preview');
    await page.locator('dialog[open] button').click();
    await page.evaluate(()=>chatRedraw());
    await page.locator('#chat-box .user img[alt="chat-image.png"]').waitFor();
    await page.evaluate(()=>{
      chatAddBubble('bot','![chart](data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aK1sAAAAASUVORK5CYII=)');
      chatAddBubble('bot','![unsafe](javascript:alert(1)) <script>alert(1)</script>');
    });
    assert.equal(await page.locator('#chat-box img[alt="chart"]').count(),1,'assistant Markdown images are rendered');
    assert.equal(await page.locator('#chat-box img[alt="unsafe"],#chat-box script').count(),0,'unsafe image URLs and HTML remain plain text');
    const imageArtifacts=path.resolve(__dirname,'../build/ui-progress-qa'); fs.mkdirSync(imageArtifacts,{recursive:true});
    await page.screenshot({path:path.join(imageArtifacts,'chat-images.png')});

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
    console.log('PASS: new-chat click reconciles old task state from the latest journal record, genuine failures and stale RUNNING tasks are classified correctly, session creation errors, image upload, attachment session isolation, upload-before-send, desktop/mobile auto-scroll and manual history position.');
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
