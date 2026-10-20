const {chromium}=require('playwright');
const assert=require('node:assert/strict');
(async()=>{
 const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});
 try{
  const page=await browser.newPage({viewport:{width:1280,height:800}});
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.goto(process.env.UI_TEST_URL);
  await page.locator('#nav-newchat').click();
  await page.waitForFunction(()=>CHAT_SESSION!=='default' && CHAT_NEW_SESSION_REQUEST===null);
  const first=await page.evaluate(()=>CHAT_SESSION);
  await page.locator('#nav-newchat').click();
  await page.waitForFunction(id=>CHAT_SESSION!==id && CHAT_NEW_SESSION_REQUEST===null,first);
  const second=await page.evaluate(()=>CHAT_SESSION);assert.notEqual(first,second);
  assert.equal(await page.locator('#chat-box .chat-message').count(),0);
  await page.waitForFunction(()=>!!CHAT_SESSION_NEXT);
  const next=await page.evaluate(()=>CHAT_SESSION_NEXT);
  await page.locator('#recent-older').click();
  await page.waitForFunction(cursor=>CHAT_SESSION_CURSOR===cursor,next);
  assert.equal(await page.evaluate(()=>CHAT_SESSION),second,'paging preserves the active conversation');
  assert.ok(await page.locator('#side-recent .side-recent-row').count()<=13);
  assert.equal(await page.locator('#recent-newest').count(),1);
  await page.locator('#recent-newest').click();
  await page.waitForFunction(()=>CHAT_SESSION_CURSOR==='');
  assert.equal(await page.evaluate(()=>CHAT_SESSION),second);
  assert.deepEqual(errors,[]);
  console.log('PASS: actual backend new-chat creates unique conversations; cursor browsing preserves selection');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
