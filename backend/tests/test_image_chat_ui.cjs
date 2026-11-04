const {chromium}=require('playwright');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const http=require('node:http');

(async()=>{
  const html=fs.readFileSync(path.join(__dirname,'../apps/web/index.html'),'utf8');
  const server=http.createServer((req,res)=>{res.writeHead(200,{'Content-Type':'text/html; charset=utf-8'});res.end(html);});
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  const browser=await chromium.launch({headless:true,channel:process.env.UI_TEST_BROWSER||'msedge'});
  try{
    const page=await browser.newPage({viewport:{width:1100,height:800}});
    const errors=[],posts=[];
    page.on('pageerror',error=>errors.push(error.message));
    await page.route('**/v1/**',async route=>{
      const url=new URL(route.request().url());
      let obj=[];
      if(url.pathname==='/v1/upload'){
        obj={ok:true,id:'a'.repeat(32),name:url.searchParams.get('name'),status:'ready',kind:'image'};
      }else if(url.pathname==='/v1/uploads/preview'){
        obj={data_url:'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aK1sAAAAASUVORK5CYII=',name:'fixture.png'};
      }else if(url.pathname==='/v1/config/llm'){
        obj={provider:'openai',model:'fixture-model',configured:true,api_key_set:true};
      }else if(url.pathname==='/v1/chat/sessions'){
        obj=route.request().method()==='POST'?{id:'image-session'}:[{id:'image-session',turns:0}];
      }else if(url.pathname==='/v1/chat/history'){
        obj={turns:[],runs:[]};
      }else if(url.pathname==='/v1/chat' && route.request().method()==='POST'){
        posts.push(route.request().postDataJSON());obj={id:9123};
      }else if(url.pathname==='/v1/tasks/9123'){
        obj={id:9123,status:'DONE',output:'图片已收到'};
      }
      await route.fulfill({status:200,contentType:'application/json',body:JSON.stringify(obj)});
    });
    await page.goto(`http://127.0.0.1:${server.address().port}`);
    await page.waitForTimeout(250);
    await page.evaluate(()=>{document.body.classList.add('desktop-shell');showAppearance();});
    await page.locator('#appearance-theme').selectOption('ocean');
    assert.equal(await page.evaluate(()=>document.body.dataset.theme),'ocean','desktop theme selector remains accessible with the web top bar hidden');
    assert.equal(await page.evaluate(()=>localStorage.getItem('cognitive-os-agent-theme')),'ocean');
    await page.evaluate(()=>closeAppearance());
    const dragonTheme=await page.evaluate(()=>{
      document.body.dataset.theme='dragongirl';
      const selectors=['aside','.top','.pillbar','.card','#chat-hero'];
      const added=[];
      const surfaces=Object.fromEntries(selectors.map(selector=>{
        let element=document.querySelector(selector);
        if(!element){
          element=document.createElement('div');
          if(selector[0]==='.') element.className=selector.slice(1);
          else if(selector[0]==='#') element.id=selector.slice(1);
          document.body.appendChild(element);added.push(element);
        }
        const style=getComputedStyle(element);
        return [selector,{background:style.backgroundColor,backdrop:style.backdropFilter}];
      }));
      added.forEach(element=>element.remove());
      return {surfaces,artworkOpacity:getComputedStyle(document.body,'::after').opacity};
    });
    for(const [selector,style] of Object.entries(dragonTheme.surfaces)){
      if(['aside','.pillbar'].includes(selector))assert.notEqual(style.background,'rgba(0, 0, 0, 0)',`${selector} must provide readable contrast`);
      else assert.equal(style.background,'rgba(0, 0, 0, 0)',`${selector} leaves the artwork visible`);
      assert.equal(style.backdrop,'none',`${selector} must not blur the Dragon Girl artwork`);
    }
    assert.equal(dragonTheme.artworkOpacity,'1','Dragon Girl artwork must render at full opacity');
    await page.evaluate(()=>{
      document.body.classList.add('has-wallpaper');
      document.body.style.setProperty('--wallpaper-image','linear-gradient(red, blue)');
      chatAddBubble('user','wallpaper transparency probe');
    });
    const wallpaper=await page.evaluate(()=>({
      bubble:getComputedStyle(document.querySelector('#chat-box .chat-message.user')).backgroundColor,
      themeLayer:getComputedStyle(document.body,'::after').display,
      wallpaper:getComputedStyle(document.body,'::before').backgroundImage
    }));
    assert.equal(wallpaper.bubble,'rgba(60, 31, 58, 0.88)','user bubbles provide contrast over bright artwork');
    assert.equal(wallpaper.themeLayer,'none','theme artwork must not cover the chosen wallpaper');
    assert.ok(wallpaper.wallpaper.includes('linear-gradient'),'chosen wallpaper remains present');
    await page.locator('#chat-input').fill('请描述这张图片');
    const png=Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aK1sAAAAASUVORK5CYII=','base64');
    await page.locator('#chat-file').setInputFiles({name:'fixture.png',mimeType:'image/png',buffer:png});
    await page.waitForFunction(()=>CHAT_ATTACH.some(item=>item.kind==='image'&&item.status==='ready'));
    await page.locator('#chat-attach img[alt="fixture.png"]').waitFor();
    await page.waitForFunction(()=>document.querySelector('#chat-attach img')?.naturalWidth===1);
    const sendBox=await page.locator('#chat-send').boundingBox();
    const viewport=page.viewportSize();
    assert.ok(sendBox && viewport && sendBox.y>=0 && sendBox.y+sendBox.height<=viewport.height,
      'adding an image must keep the send button inside the viewport');
    await page.evaluate(()=>chatSubmit());
    await page.locator('#chat-box .user img[alt="fixture.png"]').waitFor();
    assert.equal(posts.length,1);
    assert.deepEqual(posts[0].attachments,['a'.repeat(32)]);
    await page.locator('#chat-box .user img[alt="fixture.png"]').click();
    assert.equal(await page.locator('dialog[open] img').count(),1,'uploaded image opens in a zoom dialog');
    await page.locator('dialog[open] button').click();
    await page.evaluate(()=>chatRedraw());
    await page.locator('#chat-box .user img[alt="fixture.png"]').waitFor();
    await page.evaluate(()=>{
      chatAddBubble('bot','![safe](data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aK1sAAAAASUVORK5CYII=)');
      chatAddBubble('bot','![bad](javascript:alert(1)) <script>alert(1)</script>');
    });
    assert.equal(await page.locator('#chat-box img[alt="safe"]').count(),1,'assistant Markdown image is rendered');
    assert.equal(await page.locator('#chat-box img[alt="bad"],#chat-box script').count(),0,'unsafe URL and HTML are not executed');
    assert.deepEqual(errors,[]);
    console.log('PASS: upload thumbnail, image submission, sent-image preview, zoom, redraw and safe assistant Markdown image.');
  }finally{
    await browser.close();
    await new Promise(resolve=>server.close(resolve));
  }
})().catch(error=>{console.error(error);process.exitCode=1;});
