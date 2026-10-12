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
      delete CHAT_TASKS['status-test']; CHAT_CACHE['status-test'][0].progress.stage='failed'; sideRecent(CHAT_TABS_CACHE);
    });
    assert.match(await page.locator('#side-recent').innerText(),/失败/);
    console.log('PASS #102: recent sessions show active and failed status');
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
