import test from 'node:test';
import assert from 'node:assert/strict';
import {cookieContext, cookieHeader} from '../dist/browser_context.js';

test('disabled cookie forwarding performs no read, even for unsupported context', async () => {
  assert.equal(await cookieHeader('https://example.test/a', {firefox:true,incognito:true}, false,
    () => { throw new Error('must not query'); }), undefined);
});
test('trusted container and exact URL use all partitions before constructing a header', async () => {
  const context = cookieContext({cookieStoreId:'firefox-container-7',incognito:false}, true);
  let query;
  const header = await cookieHeader('https://example.test/a#fragment', context, true, async value => {
    query = value; return [{name:'selected',value:'owned'}, {name:'other',value:'value',partitionKey:null}];
  });
  assert.deepEqual(query, {url:'https://example.test/a',storeId:'firefox-container-7',partitionKey:{},firstPartyDomain:null});
  assert.equal(header, 'selected=owned; other=value');
});
test('missing/private contexts fail before reading a default store', async () => {
  for (const context of [{firefox:true,incognito:false},
    {firefox:true,incognito:false,storeId:'firefox-private'}, {firefox:false,incognito:true}]) {
    await assert.rejects(cookieHeader('https://example.test/', context, true,
      () => { assert.fail('default-store read'); }));
  }
});
test('any partition or first-party identity rejects the entire URL without partial credentials', async () => {
  for (const extra of [{partitionKey:{topLevelSite:'https://other.test'}},{firstPartyDomain:'other.test'}]) {
    await assert.rejects(cookieHeader('https://example.test/', {firefox:false,incognito:false}, true,
      async query => { assert.deepEqual(query.partitionKey, {}); return [{name:'normal',value:'secret'}, {name:'partition',value:'private',...extra}]; }));
  }
});
test('unsupported partition query never retries with a weaker filter', async () => {
  let calls = 0;
  await assert.rejects(cookieHeader('https://example.test/', {firefox:false,incognito:false}, true,
    async () => { calls++; throw new Error('API failure with private detail'); }), error => !error.message.includes('private detail'));
  assert.equal(calls, 1);
});
test('invalid or oversized headers are rejected, ordinary Chrome never receives Firefox fields', async () => {
  for (const cookie of [{name:'bad;name',value:'x'},{name:'valid',value:'x\r\nOther: value'},
    {name:'valid',value:'é'.repeat(9000)}]) {
    await assert.rejects(cookieHeader('https://example.test/', {firefox:false,incognito:false}, true,
      async query => { assert.equal('firstPartyDomain' in query, false); return [cookie]; }));
  }
});

test('production background aborts credentials when forwarding is disabled during a deferred lookup', async () => {
  // Mock-only browser APIs, exercising the actual background dispatcher rather than a copy.
  let listener, enabled=true, automatic=true, unavailable=false, entered, release, held=true;
  const started=new Promise(resolve=>{entered=resolve;});
  const pending=new Promise(resolve=>{release=resolve;});
  const sent=[];
  const event=()=>({addListener() {}});
  const api={
    runtime:{id:'owned-extension',getManifest:()=>({manifest_version:3}),
      getURL:path=>'chrome-extension://owned-extension/'+path,
      onInstalled:event(),onStartup:event(),onMessage:{addListener(value){listener=value;}},
      async sendNativeMessage(_host,request){sent.push(request);if(unavailable)throw new Error('Owned host unavailable');return{version:1,requestId:request.requestId,ok:true,taskId:'a'.repeat(32)};}},
    storage:{local:{async get(defaults){return{...defaults,includeCookies:enabled,autoCapture:automatic};},
      async set(values){enabled=values.includeCookies;automatic=values.autoCapture;},async remove(){}},onChanged:event()},
    tabs:{onRemoved:event(),onUpdated:event(),async query(){return[{id:1,url:'https://example.test/page',incognito:false}];}},
    contextMenus:{onClicked:event()},downloads:{onCreated:event()},action:{async setBadgeText(){}},
    cookies:{async getAll(query){assert.deepEqual(query.partitionKey,{});entered();return held?pending:[{name:'session',value:'owned-fixture'}];}}
  };
  const previous=Object.getOwnPropertyDescriptor(globalThis,'chrome');
  Object.defineProperty(globalThis,'chrome',{value:api,configurable:true});
  try {
    await import('../dist/background.js');
    const invoke=id=>new Promise(resolve=>{
      assert.equal(listener({kind:'native',request:{version:1,requestId:id,command:'prepare',url:'https://example.test/file'}},
        {id:api.runtime.id,url:api.runtime.getURL('popup.html'),frameId:0},resolve),true);
    });
    const waiting=invoke('1'.repeat(32));await started;
    enabled=false;release([{name:'session',value:'owned-fixture'}]);
    const denied=await waiting;assert.equal(denied.ok,false);assert.equal(sent.length,0);
    enabled=true;held=false;
    const accepted=await invoke('2'.repeat(32));assert.equal(accepted.ok,true);
    assert.equal(sent.length,1);assert.equal(sent[0].cookie,'session=owned-fixture');
    unavailable=true;
    const settings=()=>new Promise(resolve=>listener({kind:'preferences',autoCapture:true,includeCookies:false,pagePanel:false},
      {id:api.runtime.id,url:api.runtime.getURL('popup.html'),frameId:0},resolve));
    assert.equal((await settings()).ok,true);assert.equal(enabled,false);assert.equal(sent.length,1);
    automatic=false;
    assert.equal((await settings()).ok,false);assert.equal(automatic,false);
    assert.equal(sent.at(-1).command,'ping');
  } finally {
    if(previous)Object.defineProperty(globalThis,'chrome',previous);else delete globalThis.chrome;
  }
});
