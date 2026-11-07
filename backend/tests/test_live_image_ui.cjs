const {chromium}=require('playwright');
const assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path');
(async()=>{
 const [url,id]=process.argv.slice(2);
 const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});
 try{
  const page=await browser.newPage();
  if(url.endsWith('.json')){
   const report=JSON.parse(fs.readFileSync(url,'utf8'));
   const png=fs.readFileSync(url.replace(/\.json$/,'.png'));
   await page.route('http://image.test/**',route=>{
    const p=new URL(route.request().url()).pathname;
    if(p==='/')return route.fulfill({contentType:'text/html',body:fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8')});
    const data=p.startsWith('/v1/tasks/')?report.task:p==='/v1/uploads/preview'?{data_url:'data:image/png;base64,'+png.toString('base64')}:[];
    return route.fulfill({contentType:'application/json',body:JSON.stringify(data)});
   });
   await page.goto('http://image.test/');
  }else await page.goto(url);
  await page.waitForTimeout(1000);
  await page.evaluate(async id=>{
   const task=await (await fetch('/v1/tasks/'+id)).json();
   chatAddBubble('bot','');const entry=CHAT_MSGS.at(-1);
   entry.text=task.output;entry.pending=false;entry.progress=task;renderChatEntry(entry);
  },id);
  for(const width of [1280,390]){
   await page.setViewportSize({width,height:850});
   await page.waitForFunction(()=>document.querySelector('.chat-answer img')?.naturalWidth===240);
   const image=page.locator('.chat-answer img').last();const box=await image.boundingBox();
   assert.ok(box.width>0&&box.height>0&&box.width<=width);
   assert.ok(Math.abs(box.width/box.height-2)<.05,'real image preserves aspect ratio');
   await image.click();await page.locator('dialog[open] img').waitFor();
   await page.locator('dialog[open] button').click();await page.evaluate(()=>chatRedraw());
  }
  await page.waitForFunction(()=>document.querySelector('.chat-answer img')?.naturalWidth===240);
  console.log('PASS: live API image desktop/mobile rendering, zoom and redraw');
 }finally{await browser.close();}
})().catch(e=>{console.error(e);process.exitCode=1});
