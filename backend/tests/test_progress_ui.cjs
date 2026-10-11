/* Run against a local web console with Playwright available via NODE_PATH.
 * UI_TEST_URL selects the server; screenshots go to build/ui-progress-qa. */
const {chromium} = require('playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

(async () => {
  const artifacts = path.resolve(__dirname, '../build/ui-progress-qa');
  fs.mkdirSync(artifacts, {recursive:true});
  const browser = await chromium.launch({headless:true,
    ...(process.env.UI_TEST_BROWSER ? {channel:process.env.UI_TEST_BROWSER} : {})});
  try {
    const page = await browser.newPage({viewport:{width:1280,height:900}});
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.route('**/v1/**', route => {
      const url = route.request().url();
      const data = url.includes('/config/llm') ? {provider:'openai',model:'mock',configured:true,api_key_set:true} :
        url.includes('/chat/history') ? {turns:[],runs:[]} : [];
      return route.fulfill({status:200,contentType:'application/json',body:JSON.stringify(data)});
    });
    await page.goto(process.env.UI_TEST_URL || 'http://127.0.0.1:18771');
    await page.waitForTimeout(400);
    await page.evaluate(() => {
      applyTheme('light');
      chatAddBubble('user','分析 SSH 登录问题，修复并验证会话复用。');
      const el = chatAddBubble('bot','');
      const entry = CHAT_MSGS[CHAT_MSGS.length-1];
      entry.pending = true; entry.taskId = 99001;
      entry.stageSince = Date.now()-12000;
      entry.progress = {stage:'planning',seq:10,started_ms:Date.now()-26000,round:2,
        model:'deepseek-chat',model_call:2,tool_calls:2,planned_tools:['file_read','file_edit'],
        steps:[{id:101,tool:'file_read',target:'src/action/ssh_vault.c',ok:1,ms:4,args:'PRIVATE_TOOL_DETAIL_READ',out:'读取成功'},
          {id:102,tool:'file_edit',target:'src/action/tool_shell.c',ok:1,ms:8,args:'PRIVATE_TOOL_DETAIL_EDIT',out:'修改完成'}]};
      entry.progressEvents = [
        {seq:1,stage:'analyzing',elapsed_ms:0},
        {seq:3,stage:'plan_ready',planned_tools:['file_read','file_edit'],round:1,elapsed_ms:2000},
        {seq:10,stage:'planning',round:2,elapsed_ms:14000}
      ];
      CHAT_CACHE[CHAT_SESSION] = CHAT_MSGS;
      CHAT_ENTRIES.set(99001,{session:CHAT_SESSION,entry});
      window.testProgressEntry = entry;
      renderChatEntry(entry);
    });
    assert.equal(await page.locator('.run-details').evaluate(el=>el.open),false);
    assert.equal(await page.locator('.run-outline').isVisible(),true);
    assert.equal(await page.locator('.run-step-detail').first().isVisible(),false);
    const waitBefore = await page.locator('.run-stage-time').textContent();
    await page.waitForFunction(before => document.querySelector('.run-stage-time')?.textContent !== before, waitBefore, {timeout:5000});
    assert.notEqual(await page.locator('.run-stage-time').textContent(),waitBefore);
    await page.screenshot({path:path.join(artifacts,'light-collapsed.png')});
    await page.locator('.run-details > summary').click();
    await page.locator('.run-step-detail[data-step="102"] > summary').click();
    await page.evaluate(() => {
      const entry = window.testProgressEntry;
      window.testDetailsNode = entry.el.querySelector('.run-details');
      chatApplyProgress(CHAT_SESSION,99001,{...entry.progress});
    });
    assert.equal(await page.evaluate(()=>window.testDetailsNode===window.testProgressEntry.el.querySelector('.run-details')),true);
    await page.evaluate(() => {
      const entry = window.testProgressEntry;
      const steps = [entry.progress.steps[1],{id:103,tool:'ssh',ok:0,ms:80,args:'SSH_DETAIL',out:'认证失败'}];
      chatApplyProgress(CHAT_SESSION,99001,{...entry.progress,seq:11,stage:'recovering',steps,tool_calls:3});
    });
    assert.equal(await page.locator('.run-details').evaluate(el=>el.open),true);
    assert.equal(await page.locator('.run-step-detail[data-step="102"]').evaluate(el=>el.open),true);
    assert.equal(await page.locator('.run-step-detail[data-step="103"]').evaluate(el=>el.open),false);
    await page.evaluate(()=>chatRedraw());
    assert.equal(await page.locator('.run-details').evaluate(el=>el.open),true);
    assert.equal(await page.locator('.run-step-detail[data-step="102"]').evaluate(el=>el.open),true);
    await page.screenshot({path:path.join(artifacts,'light-expanded.png')});
    await page.evaluate(()=>chatFinish(CHAT_SESSION,99001,window.testProgressEntry,'认证失败，执行记录已保留。','failed'));
    assert.equal(await page.locator('.run-details').evaluate(el=>el.open),true);
    assert.equal(await page.locator('.chat-answer').last().textContent(),'认证失败，执行记录已保留。');
    await page.locator('.run-details > summary').click();
    await page.evaluate(()=>applyTheme('dark'));
    await page.screenshot({path:path.join(artifacts,'dark-failed.png')});
    await page.setViewportSize({width:390,height:844});
    await page.screenshot({path:path.join(artifacts,'mobile.png')});
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true);
    assert.deepEqual(errors,[]);
    console.log('PASS: collapsed details, live wait clock, stable expansion, step rollover, redraw, failure, themes and mobile.');
  } finally { await browser.close(); }
})().catch(error => {console.error(error);process.exitCode=1;});
