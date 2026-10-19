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
  try {
    const page=await browser.newPage({viewport:{width:1280,height:900}});
    const errors=[],polls=[],resumes=[];
    let resumed=false,phase=1,failPoll=false;
    page.on('pageerror',e=>errors.push(e.message));
    await page.route('**/v1/**',async route=>{
      const p=new URL(route.request().url()).pathname;
      let obj=[],status=200;
      if(p==='/v1/flows/list') obj={records:[
        ...(resumed?[{id:8,task_id:0,status:'RUNNING',name:'继续后的协作',created_ms:1,updated_ms:1}]:[]),
        {id:0,task_id:47,status:'FAILED',name:'原协作',created_ms:1,updated_ms:1,dag:JSON.stringify({nodes:[],edges:[]})}
      ]};
      else if(p==='/v1/flows/0/resume'){resumes.push(p);resumed=true;obj={ok:true,id:0,resumed_from:0};}
      else if(p==='/v1/tasks/0'){
        polls.push(p);
        if(failPoll){status=503;obj={error:'temporarily offline'};}
        else obj={id:0,status:phase===3?'DONE':'RUNNING',output:phase===3?'核对完成 <script>不执行</script>':'',flow_nodes:[
          {id:'step1',agent:'reviewer',status:phase===3?'ok':'running',round:phase===1?1:4,llm_calls:phase===1?1:4,tool_calls:phase===1?1:7,tokens_in:100,tokens_out:20,cur_tool:phase===1?'file_read':'grep',steps:[{tool:phase===1?'file_read':'grep',args:'{"path":"flow.c"}',ok:1,out:'匹配结果'}]},
          {id:'step2',agent:'tester',status:phase===3?'ok':'queued',steps:[]}
        ]};
      } else if(p==='/v1/tasks/47'){polls.push(p);obj={status:'DONE',output:'不得覆盖历史状态'};}
      else if(p==='/v1/chat/sessions') obj=route.request().method()==='POST'?{id:'fixture'}:[{id:'fixture',turns:0}];
      else if(p==='/v1/config/llm') obj={provider:'mock',model:'mock',configured:true};
      else if(p==='/v1/chat/history') obj={turns:[],runs:[]};
      await route.fulfill({status,contentType:'application/json',body:JSON.stringify(obj)});
    });
    await page.goto(`http://127.0.0.1:${server.address().port}`);
    await page.evaluate(()=>switchPanel('agents'));
    await page.evaluate(()=>WS.onmessage({data:JSON.stringify({type:'event',source:'flow',payload:{stage:'done',node:'step2',agent:'coder',detail:'error'}})}));
    assert.match(await page.locator('#flow-out').innerText(),/✗ \[step2:coder\] 执行失败/,'failed node must not carry a success checkmark');
    await page.locator('#flow-task-0 button').filter({hasText:'继续'}).click();
    const progress=page.locator('#flow-task-8 .flow-record-progress');
    await progress.filter({hasText:'正在执行 file_read'}).waitFor();
    assert.deepEqual(resumes,['/v1/flows/0/resume']);
    assert.match(await progress.innerText(),/执行任务 #0/,'scheduler id zero is valid, distinct from persistent record id');
    assert.match(await progress.innerText(),/等待中.*step2/);
    assert.match(await progress.innerText(),/reviewer.*模型请求 1.*工具 1.*token 120/);
    assert.match(await page.locator('#flow-task-0').innerText(),/失败/,'old failed record remains failed');
    assert.equal(polls.includes('/v1/tasks/47'),false,'do not reclassify historical tasks using a potentially reused scheduler id');
    await page.locator('#flow-task-0 button').filter({hasText:'修改'}).click();
    await page.locator('#flow-task-edit-0 input').first().fill('保留编辑草稿');
    phase=2;
    await page.waitForFunction(()=>document.querySelector('#flow-task-8 .flow-record-progress')?.textContent.includes('第4轮'));
    assert.match(await progress.innerText(),/工具 7/,'automatic poll updates without Refresh');
    assert.equal(await page.locator('#flow-task-edit-0 input').first().inputValue(),'保留编辑草稿','poll does not redraw an editor');
    failPoll=true;
    await page.waitForFunction(()=>document.querySelector('#flow-task-8 .flow-record-progress')?.textContent.includes('自动重试'));
    failPoll=false;
    await page.waitForFunction(()=>document.querySelector('#flow-task-8 .flow-record-progress')?.textContent.includes('第4轮'));
    await page.evaluate(()=>switchPanel('chat'));
    await page.evaluate(()=>switchPanel('agents'));
    await page.waitForFunction(()=>document.querySelector('#flow-task-8 .flow-record-progress')?.textContent.includes('第4轮'),null,{timeout:6000}).catch(async e=>{console.log(await page.evaluate(()=>({current,busy:FLOW_TASK_PROGRESS_BUSY,text:$('flow-tasks')?.innerText,errors:document.body.innerText.slice(-500)})));throw e;});
    phase=3;
    await page.waitForFunction(()=>document.querySelector('#flow-task-8 .flow-record-status')?.textContent.includes('已完成'));
    assert.equal(await page.locator('#flow-task-8 button').filter({hasText:'取消'}).count(),0);
    await page.locator('#flow-task-8 summary').click();
    assert.match(await progress.innerText(),/核对完成 <script>不执行<\/script>/,'output is escaped and available');
    assert.equal(await progress.locator('script').count(),0);
    const count=polls.length;
    await page.waitForTimeout(2300);
    assert.equal(polls.length,count,'terminal task stops polling');
    const output=path.join(__dirname,'../build/flow-resume-ui');fs.mkdirSync(output,{recursive:true});
    await page.screenshot({path:path.join(output,'desktop.png'),fullPage:true});
    await page.setViewportSize({width:390,height:844});
    await page.screenshot({path:path.join(output,'mobile.png'),fullPage:true});
    assert.deepEqual(errors,[]);
    console.log('PASS: actual Continue click, scheduler id 0, node progress, automatic refresh, editor preservation, transient failure recovery, panel return, terminal output and stopped polling.');
  } finally {await browser.close();await new Promise(resolve=>server.close(resolve));}
})().catch(e=>{console.error(e);process.exitCode=1;});
