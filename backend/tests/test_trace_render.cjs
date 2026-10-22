// Run with Node and Playwright installed; tests the production Trace renderer.
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { chromium } = require('playwright');
(async () => {
  const html = fs.readFileSync(path.join(__dirname, '../apps/web/index.html'), 'utf8');
  const start = html.indexOf('function renderTraceSpans(');
  const end = html.indexOf('async function loadTrace', start);
  assert(start >= 0 && end > start);
  const browser = await chromium.launch({headless: true, ...(process.env.COA_BROWSER_CHANNEL ? {channel: process.env.COA_BROWSER_CHANNEL} : {})});
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
    console.log('PASS: Trace command, result, elapsed time, failure, truncation and safe text rendering');
  } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
