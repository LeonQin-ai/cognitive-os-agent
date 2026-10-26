const {chromium}=require('playwright'),fs=require('node:fs'),path=require('node:path'),assert=require('node:assert/strict');
(async()=>{const b=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});try{
 const p=await b.newPage();let finish;await p.route('http://ui.test/**',async r=>{
 const u=new URL(r.request().url());if(u.pathname==='/')return r.fulfill({contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
 if(u.pathname==='/v1/workspaces/browse'){await new Promise(resolve=>finish=resolve);return r.fulfill({contentType:'application/json',body:JSON.stringify({path:'D:/chosen after 25s'})});}
 return r.fulfill({contentType:'application/json',body:JSON.stringify(u.pathname==='/v1/chat/sessions'?{id:'s'}:[])});});
 await p.goto('http://ui.test/');await p.clock.install();await p.evaluate(()=>{workspaceDialog();window.pickDone=false;workspaceBrowse().then(()=>pickDone=true);});
 while(!finish)await new Promise(r=>setTimeout(r,10));await p.clock.fastForward(25000);
 assert.equal(await p.evaluate(()=>pickDone),false,'folder picker remains pending beyond 20-second default timeout');
 finish();await p.waitForFunction(()=>pickDone);assert.equal(await p.locator('#workspace-path').inputValue(),'D:/chosen after 25s');
 assert.equal(await p.locator('#workspace-error').innerText(),'');console.log('PASS #216: native folder picker response after 25s is accepted without AbortError');
}finally{await b.close();}})().catch(e=>{console.error(e);process.exitCode=1;});
