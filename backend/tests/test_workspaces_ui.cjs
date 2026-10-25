const {chromium}=require('playwright');
const fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
(async()=>{
 const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});
 try{
  const page=await browser.newPage({viewport:{width:1280,height:900}}),errors=[],sessions=[],created=[];
  const workspaces=[{path:'D:/项目 A',name:'项目 A'},{path:'D:/project B',name:'project B'}];
  page.on('pageerror',e=>errors.push(e.message));
  await page.route('http://ui.test/**',async route=>{
   const r=route.request(),u=new URL(r.url());let obj=[],status=200;
   if(u.pathname==='/')return route.fulfill({contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
   if(u.pathname==='/v1/workspaces'){
    if(r.method()==='POST'){const b=r.postDataJSON();if(b.path==='missing'){status=400;obj={error:'directory unavailable'};}else workspaces.push({path:b.path,name:b.name});}
    if(status===200)obj={default_workspace:'D:/default',workspaces};
   }else if(u.pathname==='/v1/chat/sessions'){
    if(r.method()==='POST'){const b=r.postDataJSON();obj={id:'session-'+(sessions.length+1),workspace:b.workspace||'D:/default',turns:0};sessions.push(obj);created.push(b);}else obj=sessions;
   }else if(u.pathname.endsWith('/workspace')){
    const b=r.postDataJSON();sessions.find(s=>s.id===u.pathname.split('/')[4]).workspace=b.workspace;obj={ok:true};
   }else if(u.pathname==='/v1/config/llm')obj={provider:'openai',model:'fixture',configured:true};
   return route.fulfill({status,contentType:'application/json',body:JSON.stringify(obj)});
  });
  await page.goto('http://ui.test/');
  await page.waitForFunction(()=>CHAT_NEW_SESSION_REQUEST===null&&CHAT_SESSION!=='default');
  await page.waitForFunction(()=>WORKSPACES.length===2);
  await page.locator('#chat-workspace').selectOption('D:/项目 A');
  await page.waitForFunction(()=>CHAT_WORKSPACE==='D:/项目 A');
  await page.locator('#nav-newchat').click();
  await page.waitForFunction(()=>CHAT_NEW_SESSION_REQUEST===null&&CHAT_SESSION==='session-2');
  assert.equal(created.at(-1).workspace,'D:/项目 A','new chat inherits selected workspace');
  assert.equal(await page.locator('#chat-workspace').inputValue(),'D:/项目 A');
  assert.ok(await page.locator('#side-recent details.workspace-group').count()>=2);
  await page.evaluate(()=>workspaceDialog());
  await page.locator('#workspace-path').fill('missing');
  await page.locator('#workspace-dialog button[type=submit]').click();
  await page.locator('#workspace-error').getByText('directory unavailable').waitFor();
  assert.equal(await page.locator('#chat-workspace').inputValue(),'D:/项目 A','invalid directory preserves selection');
  await page.locator('#workspace-path').fill('D:/new folder');await page.locator('#workspace-name').fill('<project>');
  await page.locator('#workspace-dialog button[type=submit]').click();
  await page.waitForFunction(()=>CHAT_WORKSPACE==='D:/new folder');
  assert.equal(await page.locator('#chat-workspace option:checked').textContent(),'<project>','names render as text');
  await page.evaluate(()=>{CHAT_TASK_ID=42;chatBusyUI(true);});
  assert.equal(await page.locator('#chat-cancel').isVisible(),true);assert.equal(await page.locator('#chat-send').isVisible(),false);
  await page.evaluate(()=>{CHAT_TASK_ID=null;chatBusyUI(false);});
  assert.equal(await page.locator('#chat-send').isVisible(),true);
  assert.deepEqual(errors,[]);
  console.log('PASS: workspace selection, New Chat inheritance, grouped sidebar, invalid directory, registration, escaped labels and stop/send controls');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1;});
