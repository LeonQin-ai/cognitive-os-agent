const {chromium}=require('playwright');
const fs=require('node:fs');
const path=require('node:path');
const assert=require('node:assert/strict');

(async()=>{
  const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER || 'msedge'});
  try {
    const page=await browser.newPage({viewport:{width:1280,height:900}});
    const errors=[]; page.on('pageerror',e=>errors.push(e.message));
    let agents=[], posts=[], updates=[], fail=false;
    let sessions=[{id:11,name:'empty',kind:'group',members:[],messages:0}];
    await page.route('http://ui.test/**',async route=>{
      const req=route.request(), url=new URL(req.url()); let data=[],status=200;
      if(url.pathname==='/') return route.fulfill({contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
      if(url.pathname==='/v1/agents') data={agents};
      if(url.pathname==='/v1/im/channels') data={channels:[]};
      if(url.pathname==='/v1/im/sessions'){
        if(req.method()==='POST'){
          const body=req.postDataJSON(); posts.push(body);
          if(fail){status=400;data={error:'agent missing'};}
          else {data={id:12};sessions.push({...body,id:12,messages:0});}
        } else data={sessions};
      }
      if(url.pathname==='/v1/im/sessions/11/members'){
        const body=req.postDataJSON(); updates.push(body);
        if(fail){status=400;data={error:'save failed'};}
        else {sessions[0].members=body.members;data={ok:true};}
      }
      if(url.pathname.endsWith('/messages') && req.method()==='POST'){status=409;data={error:'choose agents first'};}
      return route.fulfill({status,contentType:'application/json',body:JSON.stringify(data)});
    });
    await page.goto('http://ui.test/');
    await page.evaluate(async()=>{switchPanel('im');await renderIm();});
    await page.locator('#im-group').check();
    assert.match(await page.locator('#im-members').innerText(),/暂无已注册/);
    await page.locator('#im-create').click();
    assert.equal(posts.length,0);
    assert.match(await page.locator('#im-member-status').innerText(),/至少选择/);
    agents=[{name:'alpha',role:'源码分析'},{name:'beta',role:'验证'}];
    await page.evaluate(()=>imLoadAgents());
    await page.locator('#im-new').fill('new room');
    await page.locator('#im-members input[value="alpha"]').check();
    fail=true; await page.locator('#im-create').click();
    await page.waitForFunction(()=>$('im-member-status').textContent==='agent missing');
    assert.equal(await page.locator('#im-new').inputValue(),'new room');
    assert(await page.locator('#im-members input[value="alpha"]').isChecked());
    fail=false; await page.locator('#im-create').click();
    await page.waitForFunction(()=>IM_ACTIVE===12);
    assert.deepEqual(posts.at(-1).members,['alpha']);
    await page.getByRole('button',{name:'编辑 empty 的成员',exact:true}).click();
    await page.locator('#im-members input[value="alpha"]').check();
    await page.locator('#im-members input[value="beta"]').check();
    fail=true; await page.getByRole('button',{name:'保存成员',exact:true}).click();
    await page.waitForFunction(()=>$('im-member-status').textContent==='save failed');
    assert(await page.locator('#im-members input[value="beta"]').isChecked());
    fail=false; await page.getByRole('button',{name:'保存成员',exact:true}).click();
    await page.waitForFunction(()=>IM_EDITING===null);
    assert.deepEqual(updates.at(-1).members,['alpha','beta']);
    await page.evaluate(()=>imSelect(11));
    await page.locator('#im-input').fill('unsent message');
    await page.evaluate(()=>imSend());
    assert.equal(await page.locator('#im-input').inputValue(),'unsent message');
    assert.match(await page.locator('#im-run-status').innerText(),/choose agents/);
    await page.setViewportSize({width:390,height:844});
    await page.getByRole('button',{name:'编辑 empty 的成员',exact:true}).click();
    const box=await page.locator('#im-members').boundingBox();
    assert(box && box.width>0 && box.x>=0 && box.x+box.width<=390,JSON.stringify(box));
    assert.deepEqual(errors,[]);
    console.log('PASS: group roster selection, empty prevention, creation/save error retention, legacy repair and mobile visibility');
  } finally {await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
