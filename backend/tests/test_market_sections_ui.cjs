const {chromium} = require('playwright');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
(async()=>{
  const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER || 'msedge'});
  try {
    const page=await browser.newPage({viewport:{width:1280,height:900}}), errors=[], requests=[];
    page.on('pageerror',e=>errors.push(e.message));
    await page.route('http://ui.test/**',route=>{
      const url=new URL(route.request().url()); requests.push(url.pathname+url.search);
      if(url.pathname==='/')return route.fulfill({contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
      let obj=[];
      if(url.pathname==='/v1/catalog/skills')obj=[{id:'greet',kind:'shell',description:'问候测试',type:'runnable'},{type:'reference',name:'Fabric',source:'https://github.com/danielmiessler/fabric',description:'参考仓库'}];
      if(url.pathname==='/v1/skills/market')obj={templates:[{name:'hello',kind:'shell',description:'模板'}],installed:[{name:'saved',kind:'prompt'}],github:[]};
      if(url.pathname==='/v1/plugins/market')obj={templates:[{name:'analyzer',description:'分析模板'}],github:[]};
      if(url.pathname==='/v1/plugins')obj={saved:{description:'已发布'}};
      return route.fulfill({contentType:'application/json',body:JSON.stringify(obj)});
    });
    await page.goto('http://ui.test/');
    await page.evaluate(async()=>{switchPanel('skills');await loadSkillsCatalog();await loadSkillsMarket();});
    const advanced=page.locator('.market-advanced'); await advanced.locator(':scope > summary').click();
    assert.equal(await advanced.locator(':scope > .card').count(),0);
    const catalog=advanced.locator(':scope > details').filter({has:page.locator('#skill-catalog')});
    await catalog.locator(':scope > summary').click();
    const runnable=page.locator('#skill-catalog > details').first();
    await runnable.locator(':scope > summary').click();
    assert.equal(await runnable.locator('button').count(),2);
    await page.evaluate(()=>renderSkillCatalog());
    assert.equal(await page.locator('#skill-catalog > details').first().getAttribute('open'),'');
    assert.equal(requests.filter(r=>r.startsWith('/v1/catalog/skillhub')).length,0);
    assert.match(await page.locator('#skh-skill-catalog').textContent(),/输入关键词/);
    await page.evaluate(()=>{
      document.querySelector('#sk-name').closest('.market-fold').open=true;
      for(let p=document.querySelector('#sk-name').parentElement;p;p=p.parentElement)if(p.tagName==='DETAILS')p.open=true;
    });
    await page.locator('#sk-name').fill('new-skill');
    assert.equal(await page.locator('#sk-name').inputValue(),'new-skill');
    await page.setViewportSize({width:390,height:844});
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1));
    await page.screenshot({path:path.join(__dirname,'../build/market-skills-mobile.png'),fullPage:true});
    await page.evaluate(async()=>{switchPanel('plugins');await loadPluginMarket();});
    await page.locator('#plugin-advanced > summary').click();
    await page.evaluate(()=>{for(let p=document.querySelector('#pl-name').parentElement;p;p=p.parentElement)if(p.tagName==='DETAILS')p.open=true;});
    await page.locator('#pl-name').fill('new-plugin');
    assert.equal(await page.locator('#pl-name').inputValue(),'new-plugin');
    assert.equal(await page.locator('#plugin-market > details').count(),3);
    assert(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1));
    assert.deepEqual(errors,[]);
    console.log('PASS: nested skill/plugin sections, refresh disclosure state, forms, empty SkillHub query and mobile layout');
  }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
