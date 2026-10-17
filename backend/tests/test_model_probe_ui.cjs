const {chromium} = require('playwright');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
(async () => {
  const browser = await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER || 'msedge'});
  try {
    const page = await browser.newPage();
    const errors=[]; page.on('pageerror',e=>errors.push(e.message));
    await page.route('http://ui.test/**',r=>r.fulfill({status:200,
      contentType:r.request().url()==='http://ui.test/'?'text/html':'application/json',
      body:r.request().url()==='http://ui.test/'?fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8'):'[]'}));
    await page.goto('http://ui.test/');
    await page.waitForTimeout(100);
    await page.evaluate(() => switchPanel('models'));
    const results=await page.evaluate(async()=>{
      const out=[], model='nvidia/nemotron-3.5-super-vl:free';
      let posts=0, polls=0;
      call=async(method,url)=>{
        if(!url.startsWith('/v1/config/llm/test')) return {status:200,obj:[]};
        if(method==='POST'){ posts++; await new Promise(r=>setTimeout(r,30)); return {status:200,obj:{pending:true,request_id:7}}; }
        polls++; return {status:200,obj:{request_id:7,pending:false,ok:true,reply:'ok',model}};
      };
      await Promise.all([testLlm({model},'路由 openrouter '),testLlm({model},'other ')]);
      out.push({case:'duplicate clicks',posts,polls,text:$('models-stat').textContent,busy:LLM_TEST_IN_FLIGHT});
      call=async(method)=>method==='POST'?{status:200,obj:{request_id:8}}:
        {status:200,obj:{request_id:8,pending:false,ok:false,error:'openai http 503: fixture unavailable'}};
      await testLlm({model},'路由 openrouter ');
      out.push({case:'provider error',text:$('models-stat').textContent});
      call=async(method)=>method==='POST'?{status:200,obj:{request_id:9}}:
        {status:200,obj:{request_id:10,pending:false,ok:true,reply:'wrong model'}};
      await testLlm({model},'');
      out.push({case:'wrong verdict',text:$('models-stat').textContent});
      call=async()=>{throw new Error('fixture transport failure');};
      await testLlm({model},'');
      out.push({case:'exception',text:$('models-stat').textContent,busy:LLM_TEST_IN_FLIGHT});
      const realNow=Date.now; let now=0; polls=0;
      Date.now=()=>now;
      call=async(method)=>{now+=25000; if(method!=='POST')polls++;
        return {status:200,obj:{request_id:11,pending:true,model}};};
      try { await testLlm({model},'路由 openrouter '); }
      finally { Date.now=realNow; }
      out.push({case:'elapsed deadline',polls,text:$('models-stat').textContent,busy:LLM_TEST_IN_FLIGHT});
      return out;
    });
    assert.equal(results[0].posts,1); assert.equal(results[0].polls,1);
    assert.match(results[0].text,/测试通过/); assert.equal(results[0].busy,false);
    assert.match(results[1].text,/503: fixture unavailable/);
    assert.match(results[2].text,/编号不匹配/); assert.doesNotMatch(results[2].text,/测试通过/);
    assert.match(results[3].text,/fixture transport failure/); assert.equal(results[3].busy,false);
    assert.match(results[4].text,/测试超时/); assert(results[4].polls<=2); assert.equal(results[4].busy,false);
    assert.deepEqual(errors,[]);
    console.log('PASS 5 model probe UI scenarios: duplicate clicks, provider errors, request identity, exception cleanup, elapsed deadline');
  } finally { await browser.close(); }
})().catch(e=>{console.error(e);process.exitCode=1;});
