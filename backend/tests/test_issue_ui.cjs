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
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
