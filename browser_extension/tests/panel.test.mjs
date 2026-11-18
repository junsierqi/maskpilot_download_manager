import test from 'node:test';
import assert from 'node:assert/strict';
import {PanelAuthority} from '../dist/panel_authority.js';

const context = {tabId:1,url:'https://example.test/page',documentId:'owned-document'};
const candidates = [1,2,3].map(n => ({url:'https://example.test/file'+n,name:'File '+n,kind:'file'}));
function fixture() {
  let tick=0, next=0;
  return {authority:new PanelAuthority(() => tick, () => (++next).toString(16).padStart(32,'0'),1000),
    advance:() => {tick+=1000;}};
}
test('page resource authority rejects foreign IDs, documents, tokens and URL changes', async () => {
  const {authority}=fixture(), model=authority.open(context,candidates);
  const forbidden=() => { assert.fail('unauthorized native action'); };
  for (const altered of [{...context,tabId:2},{...context,documentId:'reload'},{...context,url:'https://example.test/other'}])
    await assert.rejects(authority.submit(altered,model.token,[model.items[0].id],forbidden));
  await assert.rejects(authority.submit(context,'0'.repeat(32),[model.items[0].id],forbidden));
  await assert.rejects(authority.submit(context,model.token,['f'.repeat(32)],forbidden));
  await assert.rejects(authority.submit(context,model.token,[model.items[0].id,model.items[0].id],forbidden));
});
test('partial ACK retains accepted rows and lost-response retries reuse exactly the same request ID', async () => {
  const {authority}=fixture(), model=authority.open(context,candidates), calls=[];
  const ids=model.items.map(item=>item.id);
  let fail=true;
  const send=async (candidate,requestId) => {
    calls.push({url:candidate.url,requestId});
    if (candidate.url.endsWith('2') && fail) {fail=false; throw new Error('lost ACK');}
  };
  const first=await authority.submit(context,model.token,ids,send);
  assert.deepEqual(first.accepted,[ids[0]]); assert.deepEqual(first.remaining,ids.slice(1)); assert.ok(first.error);
  const second=await authority.submit(context,model.token,ids,send);
  assert.deepEqual(second.accepted,ids); assert.deepEqual(second.remaining,[]);
  assert.equal(calls.filter(call=>call.url.endsWith('1')).length,1);
  assert.equal(calls[1].requestId,calls[2].requestId);
  const refreshed=authority.open(context,candidates);
  assert.ok(refreshed.items.every(item=>item.accepted));
});
test('navigation during an ACK preserves prefix results and prevents later sends', async () => {
  const {authority}=fixture(), model=authority.open(context,candidates); let sent=0;
  const result=await authority.submit(context,model.token,model.items.map(item=>item.id),async () => {
    sent++; authority.invalidate(context.tabId);
  });
  assert.equal(sent,1); assert.equal(result.accepted.length,1); assert.equal(result.remaining.length,2); assert.ok(result.error);
});
test('concurrent clicks cannot overlap a batch and post-cookie checks observe revocation', async () => {
  const {authority}=fixture(), model=authority.open(context,candidates); let release;
  const waiting=new Promise(resolve=>{release=resolve;});
  const first=authority.submit(context,model.token,[model.items[0].id],async () => { await waiting; });
  await assert.rejects(authority.submit(context,model.token,[model.items[0].id],async()=>assert.fail()));
  authority.invalidate(context.tabId);
  assert.throws(()=>authority.assertCurrent(context,model.token));
  release(); await first;
});
test('expiry, capacity and scan bounds remove stale page authority', async () => {
  const {authority,advance}=fixture(), model=authority.open(context,candidates);
  advance(); assert.throws(()=>authority.assertCurrent(context,model.token));
  const many=authority.open(context,Array.from({length:300},(_,n)=>({url:'https://example.test/'+n,name:'Resource',kind:'file'})));
  assert.equal(many.items.length,200);
  for(let tabId=2;tabId<=65;tabId++) authority.open({...context,tabId},candidates);
  assert.throws(()=>authority.assertCurrent(context,many.token));
});
