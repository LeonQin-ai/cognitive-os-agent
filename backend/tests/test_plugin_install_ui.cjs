const {chromium} = require('playwright');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');

(async () => {
  const browser = await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER || 'msedge'});
  try {
    const page = await browser.newPage(), errors = [], jobs = new Map(), posts = [];
    let release = false, abortPoll = true;
    page.on('pageerror', error => errors.push(error.message));
    await page.route('http://ui.test/**', async route => {
      const request = route.request(), url = new URL(request.url());
      if(url.pathname === '/') return route.fulfill({contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
      let status = 200, obj = [];
      if(url.pathname.startsWith('/v1/skills/install-')){
        if(request.method() === 'POST'){
          posts.push(url.pathname);
          const id = jobs.size + 1;
          jobs.set(String(id), {path:url.pathname,fail:id === 1});
          status = 202; obj = {pending:true,job_id:id};
        } else {
          const job = jobs.get(url.searchParams.get('job_id'));
          assert.equal(job?.path, url.pathname, 'poll must target the original operation');
          if(abortPoll){ abortPoll = false; return route.abort('failed'); }
          if(!release) obj = {pending:true};
          else if(job.fail){ status = 502; obj = {error:'fixture download failed'}; }
          else obj = {ok:true,name:'fixture-installed'};
        }
      } else if(url.pathname === '/v1/config/llm') obj = {provider:'openai',model:'fixture',configured:true};
      else if(url.pathname === '/v1/plugins') obj = {};
      else if(url.pathname === '/v1/plugins/market') obj = {templates:[{name:'available-plugin',description:'Still available during download'}],github:[]};
      else if(url.pathname === '/v1/skills/market') obj = {templates:[],installed:[],github:[]};
      else if(url.pathname === '/v1/catalog/github-search') obj = [{repo:'fixture/skill',url:'https://github.com/fixture/skill',description:'fixture',stars:1}];
      return route.fulfill({status,contentType:'application/json',body:JSON.stringify(obj)});
    });
    await page.goto('http://ui.test/');
    await page.evaluate(async () => {
      switchPanel('skills');
      $('skill-search').value = 'fixture';
      await githubSearchOnline();
      $('gh-online-catalog').closest('details').open = true;
    });
    await page.locator('#gh-online-catalog button').click();
    await page.waitForFunction(() => SKILL_INSTALL_PENDING.size === 1);
    // Duplicate clicks share one download, even while its poll temporarily fails.
    await page.evaluate(() => { void installRemoteSkill('/v1/skills/install-github',{repo:'fixture/skill'}); switchPanel('plugins'); });
    await page.locator('#plugin-featured').getByText('available-plugin',{exact:true}).waitFor();
    await page.evaluate(() => pingServer());
    assert.equal(await page.locator('#connText').textContent(), '已连接');
    await page.waitForTimeout(600);
    assert.equal(posts.length, 1);
    assert.equal(await page.evaluate(() => SKILL_INSTALL_PENDING.size), 1);
    release = true;
    await page.waitForFunction(() => SKILL_INSTALL_PENDING.size === 0);
    assert.match(await page.locator('#toast').textContent(), /安装失败.*fixture download failed/);
    assert.equal(await page.locator('#connText').textContent(), '已连接');
    const completed = await page.evaluate(async () => Promise.all([
      installRemoteSkill('/v1/skills/install-remote',{repo:'fabric',id:'fab_summarize'}),
      installRemoteSkill('/v1/skills/install-skillhub',{slug:'fixture-skill'})
    ]));
    assert(completed.every(result => result.status === 200 && result.obj.ok));
    assert.equal(posts.length, 3);
    assert.equal(await page.evaluate(() => SKILL_INSTALL_PENDING.size), 0);
    assert.deepEqual(errors, []);
    console.log('PASS: click GitHub install, duplicate click, transient poll failure, panel navigation, isolated download failure and concurrent source results.');
  } finally { await browser.close(); }
})().catch(error => {console.error(error);process.exitCode = 1;});
