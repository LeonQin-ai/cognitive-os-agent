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
    const preset=await page.evaluate(async()=>{
      call=async()=>({status:200,obj:[]});
      await loadModels();
      const select=$('r-provider');
      const assertNvidia = [...select.options].filter(o=>o.textContent.includes('NVIDIA'));
      if(assertNvidia.length!==1) throw new Error('Expected one NVIDIA preset');
      select.value='nvidia';
      select.dispatchEvent(new Event('change'));
      return {model:$('r-model').value,base:$('r-base').value,provider:presetProvider()};
    });
    assert.deepEqual(preset,{model:'z-ai/glm-5.3-flash',base:'https://integrate.api.nvidia.com/v1',provider:'openai'});
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
      now=0; polls=0;
      Date.now=()=>now;
      call=async(method)=>{now+=90000; if(method!=='POST')polls++;
        return method==='POST'?{status:200,obj:{request_id:12}}:
          {status:200,obj:{request_id:12,pending:true,model:'deepseek-ai/deepseek-v4.1-flash'}};};
      try { await testLlm({model:'deepseek-ai/deepseek-v4.1-flash',base_url:'https://integrate.api.nvidia.com/v1'},'NVIDIA '); }
      finally { Date.now=realNow; }
      out.push({case:'NVIDIA extended deadline',polls,text:$('models-stat').textContent,busy:LLM_TEST_IN_FLIGHT});
      return out;
    });
    assert.equal(results[0].posts,1); assert.equal(results[0].polls,1);
    assert.match(results[0].text,/测试通过/); assert.equal(results[0].busy,false);
    assert.match(results[1].text,/503: fixture unavailable/);
    assert.match(results[2].text,/编号不匹配/); assert.doesNotMatch(results[2].text,/测试通过/);
    assert.match(results[3].text,/fixture transport failure/); assert.equal(results[3].busy,false);
    assert.match(results[4].text,/测试超时/); assert(results[4].polls<=2); assert.equal(results[4].busy,false);
    assert.match(results[5].text,/测试超时/); assert(results[5].polls>=3); assert.equal(results[5].busy,false);
    assert.deepEqual(errors,[]);
    console.log('PASS 6 model probe UI scenarios: NVIDIA preset, duplicate clicks, provider errors, request identity, exception cleanup, provider-aware deadlines');
  } finally { await browser.close(); }
})().catch(e=>{console.error(e);process.exitCode=1;});
