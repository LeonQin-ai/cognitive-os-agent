const {chromium}=require('playwright');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
(async()=>{
 const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});
 try{
  const page=await browser.newPage(),errors=[],reads=[],posts=[];
  let getGate=null,postGate=null,failGet=false,failPost=false;
  let messages={1:[{id:1,role:'user',content:'existing message'}],2:[{id:2,role:'user',content:'other group'}]};
  page.on('pageerror',e=>errors.push(e.message));
  await page.route('http://ui.test/**',async route=>{
   const r=route.request(),u=new URL(r.url());let obj=[],status=200;
   if(u.pathname==='/')return route.fulfill({contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
   const match=u.pathname.match(/^\/v1\/im\/sessions\/(\d+)\/messages$/);
   if(match){const id=Number(match[1]);
    if(r.method()==='GET'){
     const snapshot=messages[id].slice();reads.push(id);const gate=getGate;if(gate)await gate;
     if(failGet){obj={error:'fixture unavailable'};status=503;}else obj=snapshot;
    }else{
     posts.push(r.postDataJSON());const gate=postGate;if(gate)await gate;
     if(failPost){obj={error:'fixture send failed'};status=503;}else obj={ok:true,id:3,task_id:0};
    }
   }else if(u.pathname==='/v1/tasks/0')obj={status:'DONE',im_status:'done'};
   else if(u.pathname==='/v1/im/sessions')obj={sessions:[{id:1,kind:'group',members:['a','b','c'],discussion_rounds:5},{id:2,kind:'group',members:['a','b','c'],discussion_rounds:5}]};
   return route.fulfill({status,contentType:'application/json',body:JSON.stringify(obj)});
  });
  await page.goto('http://ui.test/');
  await page.evaluate(async()=>{switchPanel('im');await renderIm();IM_ACTIVE=1;});
  await page.evaluate(()=>renderImThread());assert.match(await page.locator('#im-thread').innerText(),/existing message/);
  let release;getGate=new Promise(r=>release=r);const baseline=reads.length;
  await page.evaluate(()=>{void renderImThread();void renderImThread();void renderImThread();});
  await page.waitForFunction(()=>$('im-thread').getAttribute('aria-busy')==='true');
  assert.match(await page.locator('#im-thread').innerText(),/existing message/,'refresh erased visible messages');
  assert.equal(reads.length,baseline+1,'overlapping refreshes issued duplicate requests');
  messages[1].push({id:3,role:'assistant',content:'new response'});getGate=null;release();
  await page.waitForFunction(()=>$('im-thread').textContent.includes('new response')&&$('im-thread').getAttribute('aria-busy')==='false');
  assert.equal(reads.length,baseline+2,'coalesced refresh must fetch new messages');
  getGate=new Promise(r=>release=r);await page.evaluate(()=>{void renderImThread();});
  await page.waitForFunction(()=>$('im-thread').getAttribute('aria-busy')==='true');
  getGate=null;await page.evaluate(()=>imSelect(2));
  await page.getByText('other group',{exact:false}).first().waitFor();release();
  await page.waitForTimeout(50);assert.doesNotMatch(await page.locator('#im-thread').innerText(),/existing message|new response/);
  failGet=true;await page.evaluate(()=>renderImThread());
  assert.match(await page.locator('#im-thread').innerText(),/other group/);assert.match(await page.locator('#im-thread [role=alert]').innerText(),/fixture unavailable/);
  assert.equal(await page.locator('#im-thread .spin').count(),0,'failure leaves loading spinner');
  failGet=false;await page.locator('#im-thread').getByRole('button',{name:'重试',exact:true}).click();
  await page.waitForFunction(()=>!$('im-thread').querySelector('[role=alert]'));
  let releasePost;postGate=new Promise(r=>releasePost=r);
  await page.locator('#im-input').fill('topic');await page.evaluate(()=>{void imSend();void imSend();});
  await page.waitForFunction(()=>$('im-send').disabled);assert.equal(posts.length,1);
  await page.locator('#im-input').fill('next draft');postGate=null;releasePost();
  await page.waitForFunction(()=>!$('im-send').disabled);
  assert.equal(await page.locator('#im-input').inputValue(),'next draft','ack erased newer draft');
  failPost=true;await page.evaluate(()=>imSend());
  assert.match(await page.locator('#im-run-status').innerText(),/fixture send failed/);
  assert.equal(await page.locator('#im-input').inputValue(),'next draft');assert.equal(await page.locator('#im-send').isDisabled(),false);
  assert.deepEqual(errors,[]);
  console.log('PASS: IM refresh keeps messages, single-flight/coalesced updates, group-switch isolation, explicit retry, no stuck spinner, duplicate-send prevention and draft preservation');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});

