const {chromium} = require('playwright');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
(async()=>{
  const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER || 'msedge'});
  try {
    const page=await browser.newPage();
    await page.route('http://ui.test/**',route=>route.fulfill({status:200,
      contentType:route.request().url()==='http://ui.test/'?'text/html':'application/json',
      body:route.request().url()==='http://ui.test/'?fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8'):'[]'}));
    await page.goto('http://ui.test/');
    await page.evaluate(()=>{
      chatAddBubble('bot','');
      const entry=CHAT_MSGS[CHAT_MSGS.length-1];
      entry.pending=false; entry.progress={stage:'completed',steps:[{id:1,tool:'file_write',ok:1,
        target:'st_diag_step1.py',args:'{"path":"st_diag_step1.py","content":"WRITE_DETAILS_MARKER"}',out:'wrote 20 bytes'}]};
      renderChatEntry(entry);
    });
    const file=page.locator('.run-file-detail');
    assert.equal(await file.count(),1);
    await page.locator('.run-details > summary').click();
    await file.locator('summary').click();
    assert.equal(await file.getAttribute('open'),'');
    assert.match(await file.locator('pre').innerText(),/WRITE_DETAILS_MARKER/);
    console.log('PASS #98: written-file cards expose write details on click');
    await page.evaluate(()=>{
      CHAT_TABS_CACHE=[{id:'status-test',title:'执行中的任务'}];
      CHAT_TASKS['status-test']=999;
      CHAT_CACHE['status-test']=[{role:'bot',progress:{stage:'planning'}}]; sideRecent(CHAT_TABS_CACHE);
    });
    assert.match(await page.locator('#side-recent').innerText(),/执行中/);
    await page.evaluate(()=>{
      CHAT_TABS_CACHE[0].title='这是一个非常长的源码阅读任务标题需要验证状态不会被遮住';
      $('side-recent').style.width='180px'; sideRecent(CHAT_TABS_CACHE);
    });
    const status=page.locator('.recent-task-status').filter({hasText:'执行中'});
    const statusBox=await status.boundingBox();
    const row=status.locator('..');
    const titleBox=await row.locator('.recent-session-title').boundingBox();
    const buttonBox=await row.boundingBox();
    assert(statusBox && titleBox && buttonBox);
    assert(statusBox.x>=buttonBox.x && statusBox.x+statusBox.width<=buttonBox.x+buttonBox.width);
    assert(statusBox.x+statusBox.width<=titleBox.x, 'Status must precede the clipped title');
    assert.equal(await status.evaluate(el=>getComputedStyle(el).flexShrink),'0');
    assert.equal(await status.evaluate(el=>getComputedStyle(el).order),'-1');
    assert.equal(await status.getAttribute('data-state'),'running');
    console.log('PASS #111: left status remains fully visible with a long title in a narrow sidebar');
    await page.evaluate(()=>{
      delete CHAT_TASKS['status-test']; CHAT_CACHE['status-test'][0].progress.stage='failed'; sideRecent(CHAT_TABS_CACHE);
    });
    assert.match(await page.locator('#side-recent').innerText(),/失败/);
    console.log('PASS #102: recent sessions show active and failed status');
    await page.route('http://ui.test/v1/flows/list',route=>route.fulfill({status:200,contentType:'application/json',
      body:JSON.stringify({records:[{id:42,status:'RUNNING',name:'很长的多Agent源码阅读任务标题'.repeat(30),created_ms:1,updated_ms:1}]})}));
    await page.evaluate(async()=>{
      switchPanel('agents'); await loadFlowTasks(); $('flow-tasks').style.width='200px';
      flowStatus('status','正在执行多 Agent 任务\n前置步骤失败，后续步骤已阻断');
    });
    const badge=page.locator('#flow-task-42 .flow-task-status');
    const badgeBox=await badge.boundingBox();
    const headingBox=await page.locator('#flow-task-42 .flow-task-heading').boundingBox();
    assert(badgeBox && headingBox && badgeBox.x===headingBox.x);
    assert.equal(await badge.innerText(),'执行中');
    assert(badgeBox.x+badgeBox.width<=headingBox.x+headingBox.width);
    assert.equal(await badge.evaluate(el=>getComputedStyle(el).flexShrink),'0');
    const flowStatus=page.locator('#flow-status');
    assert.equal(await flowStatus.evaluate(el=>getComputedStyle(el).textAlign),'left');
    assert.equal(await flowStatus.evaluate(el=>el.nextElementSibling.classList.contains('row')),true);
    const nodes=await page.evaluate(()=>flowNodesText({flow_nodes:[{id:'很长的后续节点名称',status:'blocked'}]}));
    assert.match(nodes,/^⊘ 已阻断（前置步骤未完成）/);
    console.log('PASS #118: Flow status stays left of long titles, above controls, with explicit blocked state');
    await page.route('http://ui.test/v1/tasks/journal?limit=50',route=>route.fulfill({status:200,contentType:'application/json',body:JSON.stringify([
      {id:0,status:'RUNNING',input:'old running'}, {id:0,status:'INTERRUPTED',input:'interrupted'},
      {id:1,status:'RUNNING',input:'old running'}, {id:1,status:'DONE',input:'completed'},
      {id:2,status:'RUNNING',input:'still active'}])}));
    await page.evaluate(async()=>{ $('journal-list') || document.body.insertAdjacentHTML('beforeend','<div id="journal-list"></div>'); await loadJournal(); });
    const history=await page.locator('#journal-list').innerText();
    assert.match(history,/INTERRUPTED/); assert.match(history,/DONE/); assert.doesNotMatch(history,/RUNNING|still active/);
    assert.equal(await page.locator('#journal-list > div').count(),2);
    console.log('PASS #130: history uses latest terminal/interrupted transition, not stale RUNNING entries');
    await page.evaluate(()=>{
      CHAT_SESSION='active-newest';
      sideRecent([
        {id:'created-first',title:'创建较早',created_ms:1,last_active_ms:100},
        {id:'created-last',title:'创建较晚但不活跃',created_ms:300,last_active_ms:300},
        {id:'active-newest',title:'最近访问',created_ms:2,last_active_ms:900}
      ]);
    });
    const recentTitles=await page.locator('#side-recent .recent-session-title').allInnerTexts();
    assert.deepEqual(recentTitles,['最近访问','创建较晚但不活跃','创建较早']);
    console.log('PASS #155: recent chats sort by last activity, not creation order');
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
