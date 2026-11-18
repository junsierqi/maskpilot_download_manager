import {spawn, spawnSync} from 'node:child_process';
import {createServer} from 'node:http';
import {createHash, randomBytes} from 'node:crypto';
import {copyFileSync, existsSync, mkdirSync, readFileSync, renameSync, writeFileSync} from 'node:fs';
import path from 'node:path';
import {parseArgs} from 'node:util';
import assert from 'node:assert/strict';
import {fileURLToPath} from 'node:url';

const scriptDirectory=path.dirname(fileURLToPath(import.meta.url));
const repo=path.resolve(scriptDirectory,'../..');
const {values}=parseArgs({options:{'build-directory':{type:'string',default:'out/build/windows-msvc-release'},browser:{type:'string'},'allow-unsandboxed-fixture':{type:'boolean',default:false}}});
const build=path.resolve(repo,values['build-directory']);
const desktop=path.join(build,'maskpilot.exe');
const hostBuild=path.join(build,'maskpilot_native_host.exe');
const extension=existsSync(path.join(build,'browser_extension/manifest.json'))?path.join(build,'browser_extension'):path.resolve(repo,'browser_extension/dist');
const chromium=values.browser||process.env.MASKPILOT_TEST_BROWSER;
assert(chromium,'Supply --browser <Chromium executable> or MASKPILOT_TEST_BROWSER. Use a test browser that permits loading unpacked extensions.');
const root=path.resolve(repo,'out/qa/browser-'+Date.now());mkdirSync(root,{recursive:true});
const id='epdfgbhpbpkpephcfmemmadlblpodnfo';
const origin=`chrome-extension://${id}/`;
const hostName='com.maskpilot.downloads';
const runRoot=path.join(root,'run-'+Date.now());
const dataDir=path.join(runRoot,'data'),downloads=path.join(runRoot,'downloads'),profile=path.join(runRoot,'profile');
const browserDownloads=path.join(runRoot,'browser-downloads'),hostDir=path.join(runRoot,'host');
for(const dir of [dataDir,downloads,browserDownloads,profile,hostDir,path.join(dataDir,'MaskPilot')])mkdirSync(dir,{recursive:true});
const temporaryHost=path.join(hostDir,path.basename(hostBuild));
copyFileSync(hostBuild,temporaryHost);
writeFileSync(path.join(hostDir,'native-bridge.json'),JSON.stringify({allowedOrigins:[origin],desktopPath:desktop,dataDirectory:dataDir}));
const manifestPath=path.join(hostDir,'com.maskpilot.downloads.json');
writeFileSync(manifestPath,JSON.stringify({name:hostName,description:'Isolated MaskPilot native browser test',path:temporaryHost,type:'stdio',allowed_origins:[origin]}));
writeFileSync(path.join(dataDir,'MaskPilot','MaskPilot.ini'),`[General]\ndownloadDirectory=${downloads.replaceAll('\\','/')}\nlanguage=en\ncompletionAction=0\ncloseToTray=true\n`);
const evidence={startedAt:new Date().toISOString(),root,runRoot,extensionId:id,assertions:[],events:[],httpRequests:[]};
const log=(name,detail={})=>{evidence.assertions.push({name,...detail});console.log('PASS',name,JSON.stringify(detail));};
function delay(ms){return new Promise(resolve=>setTimeout(resolve,ms));}
async function until(operation,description,timeout=30_000){const deadline=Date.now()+timeout;let last;while(Date.now()<deadline){try{const value=await operation();if(value)return value;}catch(error){last=error;}await delay(100);}throw new Error(`Timed out: ${description}${last?' '+last.message:''}`);}
function registry(mode){const result=spawnSync('powershell.exe',['-NoProfile','-ExecutionPolicy','Bypass','-File',path.join(scriptDirectory,'browser_registry.ps1'),'-Mode',mode,'-ManifestPath',manifestPath,'-BackupPath',path.join(root,'registry-backup.json')],{encoding:'utf8',windowsHide:true});if(result.status!==0)throw new Error(result.stderr||result.stdout);console.log(result.stdout.trim());}
function rows(){const code="import sqlite3,json,sys; c=sqlite3.connect('file:'+sys.argv[1]+'?mode=ro',uri=True); c.row_factory=sqlite3.Row; print(json.dumps([dict(r) for r in c.execute('SELECT id,state,url,destination,downloaded,total,error FROM tasks')]))";const result=spawnSync('python',['-c',code,path.join(dataDir,'tasks.db')],{encoding:'utf8',windowsHide:true});if(result.status!==0)throw new Error(result.stderr);return JSON.parse(result.stdout);}
class CDP {
  constructor(url){this.ws=new WebSocket(url);this.next=1;this.pending=new Map();this.listeners=new Map();this.open=new Promise((resolve,reject)=>{this.ws.addEventListener('open',resolve,{once:true});this.ws.addEventListener('error',reject,{once:true});});this.ws.addEventListener('message',event=>{const message=JSON.parse(String(event.data));if(message.id){const pending=this.pending.get(message.id);if(pending){this.pending.delete(message.id);clearTimeout(pending.timer);message.error?pending.reject(new Error(JSON.stringify(message.error))):pending.resolve(message.result);}}else{for(const callback of this.listeners.get(message.method)||[])Promise.resolve(callback(message.params)).catch(error=>{evidence.events.push({kind:'listener-error',message:error.message});});}});this.ws.addEventListener('close',()=>{for(const pending of this.pending.values()){clearTimeout(pending.timer);pending.reject(new Error('CDP connection closed'));}this.pending.clear();});}
  async send(method,params={}){await this.open;const id=this.next++;return new Promise((resolve,reject)=>{const timer=setTimeout(()=>{this.pending.delete(id);reject(new Error('CDP timeout '+method));},25_000);this.pending.set(id,{resolve,reject,timer});this.ws.send(JSON.stringify({id,method,params}));});}
  on(method,callback){if(!this.listeners.has(method))this.listeners.set(method,[]);this.listeners.get(method).push(callback);}
  async evaluate(expression){const result=await this.send('Runtime.evaluate',{expression,awaitPromise:true,returnByValue:true,userGesture:true});if(result.exceptionDetails)throw new Error(JSON.stringify(result.exceptionDetails));return result.result?.value;}
  close(){this.ws.close();}
}
const payload=Buffer.alloc(6*1024*1024);for(let i=0;i<payload.length;i++)payload[i]=i%251;
const hash=createHash('sha256').update(payload).digest('hex');
const streams=new Set();
// Synthetic credentials belong only to this isolated test profile; values are never logged.
const fixtureCookies={
  selected:{name:'maskpilot_fixture_session',value:randomBytes(24).toString('hex')},
  otherPath:{name:'maskpilot_fixture_other_path',value:randomBytes(24).toString('hex')},
  otherHost:{name:'maskpilot_fixture_other_host',value:randomBytes(24).toString('hex')}
};
const server=createServer((request,response)=>{
  const url=new URL(request.url,'http://localhost');
  if(url.pathname==='/page'){
    const route=url.searchParams.get('route')||'captured';
    const links=route==='panel'?'<a id="download" href="/manual?panel=one">First owned file</a><br><a id="download2" href="/manual?panel=two">Second owned file</a>':`<a id="download" href="/${route}" download="${route}.bin">Download synthetic file</a>`;
    const html=`<!doctype html><meta charset="utf-8"><title>MaskPilot isolated browser fixture</title><h1>Native download handoff</h1>${links}`;
    response.writeHead(200,{'Content-Type':'text/html','Content-Length':Buffer.byteLength(html)});response.end(html);return;
  }
  if(!['/manual','/captured','/fallback','/protected'].includes(url.pathname)){response.writeHead(404);response.end();return;}
  const requestInfo={url:url.pathname,range:request.headers.range||'',at:Date.now()};
  if(url.pathname==='/protected'){
    const cookies=new Map(String(request.headers.cookie||'').split(';').map(part=>{
      const equal=part.indexOf('=');return equal<0?['','']:[part.slice(0,equal).trim(),part.slice(equal+1).trim()];
    }).filter(([name])=>name));
    requestInfo.cookieNames=[...cookies.keys()].sort();
    requestInfo.authenticated=cookies.get(fixtureCookies.selected.name)===fixtureCookies.selected.value;
  }
  evidence.httpRequests.push(requestInfo);
  if(url.pathname==='/protected'&&!requestInfo.authenticated){
    response.writeHead(401,{'Content-Length':'0','Cache-Control':'no-store'});response.end();return;
  }
  let begin=0,end=payload.length-1;
  const range=/^bytes=(\d+)-(\d*)$/.exec(request.headers.range||'');
  if(range){begin=Number(range[1]);end=range[2]?Number(range[2]):end;}
  if(begin>end||end>=payload.length){response.writeHead(416,{'Content-Range':`bytes */${payload.length}`,'Content-Length':'0'});response.end();return;}
  const headers={'Content-Type':'application/octet-stream','Content-Length':String(end-begin+1),'ETag':'"browser-fixture-v1"','Accept-Ranges':'bytes','Content-Disposition':`attachment; filename="${url.pathname.slice(1)}.bin"`};
  if(range)headers['Content-Range']=`bytes ${begin}-${end}/${payload.length}`;
  response.writeHead(range?206:200,headers);
  let offset=begin;const timer=setInterval(()=>{if(response.destroyed){clearInterval(timer);streams.delete(timer);return;}const next=Math.min(end+1,offset+16384);response.write(payload.subarray(offset,next));offset=next;if(offset>end){clearInterval(timer);streams.delete(timer);response.end();}},5);streams.add(timer);
  response.on('close',()=>{clearInterval(timer);streams.delete(timer);});
});
let registered=false,desktopProcess,browserProcess,browser,worker,page,hostMoved=false;
const socketClients=[];
const processLogs={desktop:'',browser:''};
const browserEvents=[];
const env={...process.env,PATH:build+path.delimiter+(process.env.PATH||''),QT_QPA_PLATFORM:'offscreen',NO_PROXY:'127.0.0.1,localhost,::1',no_proxy:'127.0.0.1,localhost,::1'};
async function native(command,fields={}){const request={version:1,requestId:randomBytes(16).toString('hex'),command,...fields};const result=await worker.evaluate(`chrome.runtime.sendNativeMessage(${JSON.stringify(hostName)},${JSON.stringify(request)})`);assert.equal(result.version,1);assert.equal(result.requestId,request.requestId);return result;}
async function openPage(url){const target=await browser.send('Target.createTarget',{url});const targetInfo=await until(async()=>{const values=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();return values.find(value=>value.id===target.targetId);},'page target');const cdp=new CDP(targetInfo.webSocketDebuggerUrl);socketClients.push(cdp);await cdp.send('Page.enable');await until(()=>cdp.evaluate("document.readyState === 'complete'"),'document ready');return cdp;}
let port;
try {
  for(const file of [desktop,hostBuild,chromium,path.join(extension,'manifest.json')])assert(existsSync(file),file);
  registry('register');registered=true;
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));const base=`http://127.0.0.1:${server.address().port}`;
  desktopProcess=spawn(desktop,['--background','--data-dir',dataDir],{env,windowsHide:true});desktopProcess.stderr.on('data',buffer=>processLogs.desktop+=buffer.toString());desktopProcess.stdout.on('data',buffer=>processLogs.desktop+=buffer.toString());
  await until(()=>existsSync(path.join(dataDir,'ipc.token'))&&existsSync(path.join(dataDir,'tasks.db')),'desktop readiness');
  const arguments_=['--headless=new',...(values['allow-unsandboxed-fixture']?['--no-sandbox']:[]),`--load-extension=${extension}`,`--disable-extensions-except=${extension}`,`--user-data-dir=${profile}`,'--remote-debugging-port=0','--no-first-run','--no-default-browser-check','--disable-background-networking','--disable-component-update','--disable-sync','--disable-features=Translate','about:blank'];
  evidence.browserArguments=arguments_;evidence.productionSandboxVerified=!values['allow-unsandboxed-fixture'];
  browserProcess=spawn(chromium,arguments_,{env,windowsHide:true});browserProcess.stderr.on('data',buffer=>processLogs.browser+=buffer.toString());browserProcess.stdout.on('data',buffer=>processLogs.browser+=buffer.toString());
  const activePort=path.join(profile,'DevToolsActivePort');await until(()=>existsSync(activePort),'Chromium debug port');const lines=readFileSync(activePort,'utf8').trim().split(/\r?\n/);port=Number(lines[0]);browser=new CDP(`ws://127.0.0.1:${port}${lines[1]}`);socketClients.push(browser);await browser.send('Browser.setDownloadBehavior',{behavior:'allow',downloadPath:browserDownloads,eventsEnabled:true});
  browser.on('Browser.downloadWillBegin',event=>{browserEvents.push({type:'begin',...event});evidence.events.push({type:'begin',...event});});
  browser.on('Browser.downloadProgress',event=>{
    browserEvents.push({type:'progress',...event});
    if(event.state==='completed'||event.state==='canceled'){
      const entry={type:'final-progress',...event};
      if(event.state==='canceled'){entry.persistedTasks=rows().map(row=>({id:row.id,state:row.state,url:row.url}));}
      evidence.events.push(entry);
    }
  });
  const workerInfo=await until(async()=>{const values=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();return values.find(value=>value.type==='service_worker'&&value.url.startsWith(origin));},'extension service worker',20_000);
  worker=new CDP(workerInfo.webSocketDebuggerUrl);socketClients.push(worker);worker.on('Runtime.exceptionThrown',event=>evidence.events.push({type:'worker-exception',...event}));await worker.send('Runtime.enable');
  await until(()=>worker.evaluate("!!globalThis.chrome?.runtime?.sendNativeMessage"),'extension runtime initialization');
  evidence.workerContext=await worker.evaluate("({location:location.href,extensionId:chrome.runtime.id})");
  assert.equal((await native('ping')).ok,true);log('Genuine Chromium extension API reaches native host and authenticated desktop');
  const prepared=await native('prepare',{url:`${base}/manual`,filename:'manual.bin'});assert(prepared.ok,JSON.stringify(prepared));assert(/^[a-f0-9]{32}$/i.test(prepared.taskId));
  const saved=rows().find(row=>row.id===prepared.taskId);assert(saved);assert.equal(saved.state,5);assert(!existsSync(saved.destination));log('Prepare ACK arrives after SQLite durable Paused row',{taskId:prepared.taskId});
  assert.equal((await native('resume',{taskId:prepared.taskId})).ok,true);
  const manual=await until(()=>rows().find(row=>row.id===prepared.taskId&&row.state===9),'manual completion');assert.equal(createHash('sha256').update(readFileSync(manual.destination)).digest('hex'),hash);log('Native resume completes with correct synthetic bytes',{sha256:hash});
  await worker.evaluate('chrome.storage.local.set({autoCapture:true,includeCookies:false})');
  page=await openPage(`${base}/page?route=captured`);await page.evaluate("document.getElementById('download').click();true");
  const captured=await until(()=>rows().find(row=>row.url===`${base}/captured`&&row.state===9),'automatic handoff completion');assert.equal(createHash('sha256').update(readFileSync(captured.destination)).digest('hex'),hash);
  const cancelled=await until(()=>evidence.events.find(event=>event.type==='final-progress'&&event.state==='canceled'&&event.persistedTasks?.some(row=>row.id===captured.id)),'browser cancellation after persistence');assert(cancelled.persistedTasks.some(row=>row.id===captured.id));log('Real browser download cancelled only after native task persisted',{taskId:captured.id,browserGuid:cancelled.guid,sha256:hash});
  // Positive, explicitly opted-in authentication test. No disabled-cookie handoff is attempted.
  const protectedUrl=`${base}/protected`;
  const otherHostUrl=`${base.replace('127.0.0.1','localhost')}/protected`;
  assert.equal((await fetch(protectedUrl)).status,401,'Protected fixture must actually require its session cookie');
  const cookieDetails=[
    {url:protectedUrl,...fixtureCookies.selected,path:'/protected',httpOnly:true,secure:false,sameSite:'lax'},
    {url:`${base}/unrelated`,...fixtureCookies.otherPath,path:'/unrelated',httpOnly:true,secure:false,sameSite:'lax'},
    {url:otherHostUrl,...fixtureCookies.otherHost,path:'/',httpOnly:true,secure:false,sameSite:'lax'}
  ];
  try {
    await worker.evaluate(`Promise.all(${JSON.stringify(cookieDetails)}.map(details=>chrome.cookies.set(details))).then(cookies=>{if(cookies.some(cookie=>!cookie))throw new Error('Could not seed synthetic fixture cookies');return true;})`);
    const selectedNames=await worker.evaluate(`chrome.cookies.getAll({url:${JSON.stringify(protectedUrl)}}).then(cookies=>cookies.map(cookie=>cookie.name).sort())`);
    assert.deepEqual(selectedNames,[fixtureCookies.selected.name],'Cookie API must expose only cookies applicable to the selected URL');
    await worker.evaluate('chrome.storage.local.set({autoCapture:true,includeCookies:true})');
    const authenticatedPage=await openPage(`${base}/page?route=protected`);
    await authenticatedPage.evaluate("document.getElementById('download').click();true");
    const authenticated=await until(()=>rows().find(row=>row.url===protectedUrl&&row.state===9),'cookie-authenticated browser/native completion');
    assert.equal(createHash('sha256').update(readFileSync(authenticated.destination)).digest('hex'),hash);
    const cancellation=await until(()=>evidence.events.find(event=>event.type==='final-progress'&&event.state==='canceled'&&event.persistedTasks?.some(row=>row.id===authenticated.id)),'authenticated handoff cancellation after durable persistence');
    assert(cancellation.persistedTasks.some(row=>row.id===authenticated.id));
    const nativeRequests=evidence.httpRequests.filter(request=>request.url==='/protected'&&request.range);
    assert(nativeRequests.length>=2,'Expected an authenticated native probe and segmented file requests');
    for(const request of nativeRequests){
      assert.equal(request.authenticated,true,'Every native Range request must authenticate');
      assert.deepEqual(request.cookieNames,[fixtureCookies.selected.name],'Unrelated host/path cookies must never be forwarded');
    }
    const listing=await native('list');assert.equal(listing.ok,true);
    // Schema 4 stores binary torrent metadata. Preserve every column in the
    // credential audit, including UTF-8 text embedded in BLOB values.
    const sqliteCode="import sqlite3,json,sys; c=sqlite3.connect('file:'+sys.argv[1]+'?mode=ro',uri=True); c.row_factory=sqlite3.Row; print(json.dumps([dict(r) for r in c.execute('SELECT * FROM tasks')],default=lambda value:value.decode('utf-8','backslashreplace'))); c.close()";
    const dumped=spawnSync('python',['-c',sqliteCode,path.join(dataDir,'tasks.db')],{encoding:'utf8',windowsHide:true});
    assert.equal(dumped.status,0,dumped.stderr);
    const listed=JSON.stringify(listing);
    for(const cookie of Object.values(fixtureCookies)){
      assert(!dumped.stdout.includes(cookie.name)&&!dumped.stdout.includes(cookie.value),'Cookie material appeared in SQLite columns');
      assert(!listed.includes(cookie.name)&&!listed.includes(cookie.value),'Cookie material appeared in the native task list');
      for(const filename of ['tasks.db','tasks.db-wal','tasks.db-shm']){
        const candidate=path.join(dataDir,filename);if(!existsSync(candidate))continue;
        const bytes=readFileSync(candidate);
        assert(!bytes.includes(Buffer.from(cookie.value,'utf8'))&&!bytes.includes(Buffer.from(cookie.value,'utf16le')),'Cookie value appeared in SQLite or its sidecars');
      }
    }
    log('Cookie-authenticated browser handoff forwards only selected-URL credentials without persistence',{
      taskId:authenticated.id,browserGuid:cancellation.guid,sha256:hash,
      authenticatedRangeRequests:nativeRequests.length,forwardedCookieNames:[fixtureCookies.selected.name],
      excludedCookieNames:[fixtureCookies.otherPath.name,fixtureCookies.otherHost.name],sqliteAndNativeListSecretChecks:true
    });
  } finally {
    await worker.evaluate(`Promise.all(${JSON.stringify(cookieDetails.map(cookie=>({url:cookie.url,name:cookie.name})))}.map(cookie=>chrome.cookies.remove(cookie))).then(()=>true)`);
    await worker.evaluate('chrome.storage.local.set({autoCapture:true,includeCookies:false})');
  }
  try {
    const oldTargets=new Set((await(await fetch(`http://127.0.0.1:${port}/json/list`)).json()).map(target=>target.id));
    await worker.evaluate('chrome.action.openPopup()');
    const info=await until(async()=>{const targets=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();return targets.find(target=>target.url===origin+'popup.html'&&!oldTargets.has(target.id));},'actual action popup target',10_000);
    const popup=new CDP(info.webSocketDebuggerUrl);socketClients.push(popup);await popup.send('Page.enable');
    await until(()=>popup.evaluate("document.getElementById('connection')?.textContent.includes('已连接')"),'actual action popup connected state',15_000);
    const screenshot=await popup.send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});writeFileSync(path.join(root,'popup-real-chromium.png'),Buffer.from(screenshot.data,'base64'));
    log('Captured actual connected extension action-popup rendering',{file:path.join(root,'popup-real-chromium.png')});
    await browser.send('Target.closeTarget',{targetId:info.id});
  } catch(error) {evidence.popupScreenshotError=error.message;console.log('Screenshot optional boundary:',error.message);}
  await worker.evaluate('chrome.storage.local.set({autoCapture:false,includeCookies:false,pagePanel:true})');
  const panelPage=await openPage(`${base}/page?route=panel`);
  await until(()=>panelPage.evaluate("!!document.getElementById('maskpilot-download-panel')?.shadowRoot?.querySelector('#panel-toggle')"),'real page panel');
  async function panelClick(selector){
    const point=await panelPage.evaluate(`(()=>{const e=document.getElementById('maskpilot-download-panel').shadowRoot.querySelector(${JSON.stringify(selector)});const r=e.getBoundingClientRect();return{x:r.x+r.width/2,y:r.y+r.height/2};})()`);
    await panelPage.send('Input.dispatchMouseEvent',{type:'mousePressed',button:'left',clickCount:1,...point});
    await panelPage.send('Input.dispatchMouseEvent',{type:'mouseReleased',button:'left',clickCount:1,...point});
  }
  await panelClick('#panel-toggle');
  await until(()=>panelPage.evaluate("document.getElementById('maskpilot-download-panel').shadowRoot.querySelectorAll('#panel-items input').length===2"),'two bounded panel resources');
  await panelClick('#panel-items input');
  await panelPage.evaluate("(()=>{const r=document.getElementById('maskpilot-download-panel').shadowRoot;const a=[...r.querySelectorAll('input')];for(const e of a){e.checked=true;e.value=a.at(-1).value;}r.querySelector('#panel-send').click();return true;})()");
  await delay(300);assert(!rows().some(row=>row.url.includes('?panel=')),'Synthetic click or page mutation caused native action');
  await panelClick('#panel-send');
  const panelTask=await until(()=>rows().find(row=>row.url===`${base}/manual?panel=one`&&row.state===9),'trusted panel selection output');
  assert.equal(createHash('sha256').update(readFileSync(panelTask.destination)).digest('hex'),hash);
  assert(!rows().some(row=>row.url===`${base}/manual?panel=two`),'Page-mutated DOM selected an additional resource');
  const panelTab=await worker.evaluate(`chrome.tabs.query({}).then(tabs=>tabs.find(tab=>tab.url===${JSON.stringify(`${base}/page?route=panel`)}).id)`);
  const denied=await worker.evaluate(`chrome.scripting.executeScript({target:{tabId:${panelTab},frameIds:[0]},func:async()=>chrome.runtime.sendMessage({kind:'native',request:{version:1,requestId:'f'.repeat(32),command:'list'}})}).then(results=>results[0].result)`);
  assert.equal(denied.ok,false,'Page content received global native command authority');
  const screenshot=await panelPage.send('Page.captureScreenshot',{format:'png',captureBeyondViewport:false});
  writeFileSync(path.join(root,'page-panel-real-chromium.png'),Buffer.from(screenshot.data,'base64'));
  log('Trusted page panel selection ignores synthetic clicks and page-mutated checkbox authority',{taskId:panelTask.id,sha256:hash,contentNativeRejected:true});
  await worker.evaluate('chrome.storage.local.set({pagePanel:false,autoCapture:true,includeCookies:false})');
  await until(()=>panelPage.evaluate("document.getElementById('maskpilot-download-panel').hidden"),'panel disabled');
  assert(!rows().some(row=>row.url===`${base}/manual?panel=two`));
  await until(()=>{try{renameSync(temporaryHost,temporaryHost+'.unavailable');hostMoved=true;return true;}catch{return false;}},'temporarily disabling isolated native host');
  const consentUi=await openPage(origin+'popup.html');
  const revoked=await consentUi.evaluate("chrome.runtime.sendMessage({kind:'preferences',autoCapture:true,includeCookies:false,pagePanel:false})");
  assert.equal(revoked.ok,true,'Consent revocation must not require the unavailable native host');
  const fallbackPage=await openPage(`${base}/page?route=fallback`);await fallbackPage.evaluate("document.getElementById('download').click();true");
  const fallback=await until(async()=>{const items=await worker.evaluate("chrome.downloads.search({})");return items.find(item=>item.url===`${base}/fallback`&&item.state==='complete');},'browser recovery after native host unavailable',40_000);
  assert.equal(createHash('sha256').update(readFileSync(fallback.filename)).digest('hex'),hash);assert(!rows().some(row=>row.url===`${base}/fallback`));log('Unavailable native host restores browser download without native duplicate',{browserDownloadId:fallback.id,sha256:hash,consentRevocation:true});
  renameSync(temporaryHost+'.unavailable',temporaryHost);hostMoved=false;
  evidence.result='passed';
} catch(error) {
  evidence.result='failed';evidence.failure=error.stack||String(error);console.error(error);process.exitCode=1;
} finally {
  if(hostMoved){try{renameSync(temporaryHost+'.unavailable',temporaryHost);}catch(error){evidence.hostRestoreError=error.message;evidence.result='failed';process.exitCode=1;}}
  if(browser){try{await browser.send('Browser.close');}catch{}}
  for(const client of socketClients){try{client.close();}catch{}}
  for(const process_ of [browserProcess,desktopProcess])if(process_&&process_.exitCode===null){await delay(300);if(process_.exitCode===null)spawnSync('taskkill',['/PID',String(process_.pid),'/T','/F'],{windowsHide:true,stdio:'ignore'});}
  for(const timer of streams)clearInterval(timer);server.closeAllConnections();await new Promise(resolve=>server.close(resolve));
  if(registered){try{registry('restore');evidence.registryRestored=true;}catch(error){evidence.registryRestoreError=error.message;evidence.result='failed';process.exitCode=1;}}
  evidence.finishedAt=new Date().toISOString();writeFileSync(path.join(root,'evidence.json'),JSON.stringify(evidence,null,2));writeFileSync(path.join(root,'desktop.log'),processLogs.desktop);writeFileSync(path.join(root,'chromium.log'),processLogs.browser);console.log('Evidence:',path.join(root,'evidence.json'));
}
