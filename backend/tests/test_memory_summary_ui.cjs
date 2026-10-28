const {chromium}=require('playwright');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
(async()=>{
 const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});
 try{
  const page=await browser.newPage(),errors=[];page.on('pageerror',e=>errors.push(e.message));
  let phase='idle',posts=0,cancels=0,offline=false,hold=null,saved;
  const text='# 完整记忆\n<script>window.PWNED=true</script>\n'+'长摘要内容\n'.repeat(1500)+'SUMMARY_UI_TAIL';
  await page.route('http://ui.test/**',async route=>{
   const r=route.request(),url=new URL(r.url());let obj=[],status=200;
   if(url.pathname==='/')return route.fulfill({contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
   if(url.pathname==='/v1/config/llm')obj={configured:true,provider:'openai',model:'fixture',api_key_set:true};
   if(url.pathname==='/v1/memory')obj={working:['fixture'],longterm:{project:'proof'}};
   if(url.pathname==='/v1/memory/summary'){
    if(r.method()==='POST'){posts++;phase='RUNNING';obj={task_id:0};status=202;}
    else if(r.method()==='DELETE'){cancels++;phase='CANCELLED';obj={ok:true};}
    else{
     const gate=hold;if(gate)await gate;
     if(offline){status=503;obj={error:'fixture offline'};}
     else obj={...(saved?{summary:saved}:{}),...(phase==='idle'?{}:{job:{task_id:0,status:phase,...(phase==='FAILED'?{result:{error:'fixture summary failed'}}:{})}})};
    }
   }
   return route.fulfill({status,contentType:'application/json',body:JSON.stringify(obj)});
  });
  await page.goto('http://ui.test/');
  await page.evaluate(()=>{Object.defineProperty(navigator,'clipboard',{configurable:true,value:{writeText:async t=>window.copiedSummary=t}});switchPanel('memory');});
  await page.getByText('暂无成功摘要',{exact:true}).waitFor();
  assert.equal(await page.locator('#memory-summary-copy').isVisible(),false);
  await page.evaluate(()=>{void generateMemorySummary();void generateMemorySummary();});
  await page.getByText('正在生成摘要…',{exact:true}).waitFor();assert.equal(posts,1);
  assert.equal(await page.locator('#memory-summary-generate').isDisabled(),true);
  await page.locator('#memory-summary-cancel').click();
  await page.waitForFunction(()=>$('memory-summary-status').textContent.includes('已取消'));assert.equal(cancels,1);
  await page.locator('#memory-summary-generate').click();
  saved={text,generated_ms:Date.now(),model:'fixture',working_count:1,longterm_count:1,usage_available:true,tokens_in:123,tokens_out:45};phase='DONE';
  await page.waitForFunction(()=>$('memory-summary-text').textContent.endsWith('SUMMARY_UI_TAIL'));
  assert.equal(await page.locator('#memory-summary-text').textContent(),text);
  assert.equal(await page.locator('#memory-summary-text script').count(),0);assert.equal(await page.evaluate(()=>window.PWNED),undefined);
  await page.locator('#memory-summary-copy').click();assert.equal(await page.evaluate(()=>window.copiedSummary),text);
  assert.match(await page.locator('#memory-summary-meta').innerText(),/123 输入 \/ 45 输出/);
  await page.screenshot({path:path.join(__dirname,'../build/memory-summary-ui.png')});
  await page.locator('#memory-summary-generate').click();phase='FAILED';
  await page.waitForFunction(()=>$('memory-summary-status').textContent.includes('显示上次成功摘要'));
  assert.equal(await page.locator('#memory-summary-text').textContent(),text);
  offline=true;await page.evaluate(()=>loadMemorySummary());
  assert.match(await page.locator('#memory-summary-status').textContent(),/fixture offline/);
  assert.equal(await page.locator('#memory-summary-text').textContent(),text);offline=false;
  // Old in-flight replies must not overwrite a newly mounted Memory panel.
  let release;hold=new Promise(r=>release=r);await page.evaluate(()=>{void loadMemorySummary();switchPanel('dashboard');});
  hold=null;await page.evaluate(()=>switchPanel('memory'));await page.waitForFunction(()=>$('memory-summary-text').textContent.endsWith('SUMMARY_UI_TAIL'));
  release();await page.waitForTimeout(50);assert.equal(await page.locator('#memory-summary-text').textContent(),text);
  assert.deepEqual(errors,[]);console.log('PASS #233: visible full summary, token metadata, copy, task zero, duplicate prevention, cancel/failure keep prior output, retry and stale-panel isolation.');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
