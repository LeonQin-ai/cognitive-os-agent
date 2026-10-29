// Run with Node and Playwright installed; tests the production Trace renderer.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { chromium } = require('playwright');
(async () => {
  const html = fs.readFileSync(path.join(__dirname, '../apps/web/index.html'), 'utf8');
  const start = html.indexOf('let traceLoading = false;');
  const end = html.indexOf('async function load(panel)', start);
  assert(start >= 0 && end > start);
  const browser = await chromium.launch({headless: true, ...(process.env.UI_TEST_EXECUTABLE ? {executablePath:process.env.UI_TEST_EXECUTABLE} : process.env.COA_BROWSER_CHANNEL ? {channel: process.env.COA_BROWSER_CHANNEL} : {})});
  try {
    const page = await browser.newPage();
    await page.setContent('<div id="trace"></div>');
    await page.addScriptTag({content: html.slice(start, end)});
    await page.evaluate(() => renderTraceSpans(document.querySelector('#trace'), [{
      name: 'tool.shell', task_id: 42, status: -1, start_ms: 1, end_ms: 124,
      duration_ms: 123, command: '<img src=x onerror="window.injected=true">',
      result: 'exit code 7\n中文结果', result_truncated: true
    }], 'empty'));
    await page.locator('summary').click();
    assert.deepEqual(await page.locator('pre').allTextContents(), ['<img src=x onerror="window.injected=true">', 'exit code 7\n中文结果']);
    assert((await page.locator('#trace').innerText()).includes('123 ms'));
    assert((await page.locator('#trace').innerText()).includes('失败'));
    assert((await page.locator('#trace').innerText()).includes('预览已截断'));
    assert.equal(await page.locator('#trace img').count(), 0);
    assert.equal(await page.evaluate(() => window.injected), undefined);
    await page.addScriptTag({content: `const panels = {};\n${html.slice(html.indexOf('panels.trace = `'),html.indexOf('panels.im = `'))}\ndocument.body.innerHTML = panels.trace;`});
    await page.addScriptTag({content: `
      const $ = id => document.getElementById(id);
      let current = 'trace';
      let fixtureSpans = [{id:7, task_id:95, name:'llm.plan',status:1,start_ms:100,end_ms:200,duration_ms:100,command:'planner decision',result:'{"round":3}'}];
      let fixtureRecords = [];
      async function call(method, path){return {status:200,obj:path.startsWith('/v1/trace/history')?fixtureRecords:fixtureSpans};}
      function toast(message){throw new Error(message);}
      function copyText(text){window.copiedDiagnosis=text;}
    `});
    await page.evaluate(() => loadTrace());
    await page.locator('#trace-live summary').click();
    await page.evaluate(() => loadTrace());
    assert.equal(await page.locator('#trace-live details').evaluate(el => el.open), true, 'polling retains expanded live decision');
    await page.evaluate(() => {fixtureRecords=[{id:95,status:'FAILED',session:'test',ts:200,spans:fixtureSpans}];return loadTrace();});
    assert.equal(await page.locator('#trace-history details').evaluate(el => el.open), true, 'live expansion follows journaled span');
    await page.evaluate(() => loadTrace());
    assert.equal(await page.locator('#trace-history details').evaluate(el => el.open), true, 'history polling retains expansion');
    await page.locator('#trace-history summary').click();
    await page.evaluate(() => loadTrace());
    assert.equal(await page.locator('#trace-history details').evaluate(el => el.open), false, 'manual collapse is retained');
    await page.locator('#trace-history summary').click();
    await page.evaluate(() => {fixtureRecords=[{id:96,status:'FAILED',session:'other',ts:400,spans:[{...fixtureSpans[0],task_id:96,start_ms:300}]}];fixtureSpans=fixtureRecords[0].spans;return loadTrace();});
    assert.equal(await page.locator('#trace-history details').evaluate(el => el.open), false, 'another task does not inherit expansion');
    await page.locator('#trace-expand-all').click();
    assert.equal(await page.locator('#trace-history details').evaluate(el => el.open), true);
    await page.evaluate(() => loadTrace());
    assert.equal(await page.locator('#trace-history details').evaluate(el => el.open), true, 'bulk expansion survives polling');
    await page.locator('#trace-collapse-all').click();
    assert.equal(await page.locator('#trace-history details').evaluate(el => el.open), false);
    await page.locator('#trace-copy').click();
    const copied = await page.evaluate(() => JSON.parse(window.copiedDiagnosis));
    assert.equal(copied.history[0].id,96);
    assert.equal(copied.history[0].status,'FAILED');
    assert.equal(copied.history[0].spans[0].command,'planner decision');
    assert.equal(copied.history[0].spans[0].result,'{"round":3}', 'copy includes collapsed planner details');
    assert.deepEqual(copied.live_spans,[], 'journaled spans are not duplicated');
    await page.evaluate(() => {$('trace-task-id').value='96';return loadTrace();});
    await page.locator('#trace-copy').click();
    assert.equal(await page.evaluate(() => JSON.parse(window.copiedDiagnosis).task_filter),'96');
    console.log('PASS: Trace command, result, elapsed time, failure, truncation and safe text rendering');
    console.log('PASS: Trace expansion survives polling and journaling, manual collapse and task isolation');
    console.log('PASS: bulk expand/collapse and filtered diagnostic copy include collapsed details without duplicate spans');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
