import { test } from 'node:test';
import assert from 'node:assert/strict';
import { handoffDownload, safeHttpUrl, safeFilename, validateRequest, validateResponse,
  normalizeCandidates, withTimeout, newRequestId } from '../dist/protocol.js';
const ID = '0123456789abcdef0123456789abcdef';
test('recovery protocol permits only narrow source/token fields and checked resume', () => {
  const base = {version: 1, requestId: ID};
  assert.equal(validateRequest({...base, command: 'recovery.begin', taskId: ID}).taskId, ID);
  assert.equal(validateRequest({...base, command: 'recovery.apply', recoveryToken: ID,
    url: 'https://example.test/file#fragment', cookie: 'session=fixture'}).url, 'https://example.test/file');
  assert.equal(validateRequest({...base, command: 'recovery.resume', recoveryToken: ID}).recoveryToken, ID);
  for (const field of ['destination', 'proxy', 'ownerId', 'mediaDownloader', 'taskId'])
    assert.throws(() => validateRequest({...base, command: 'recovery.apply', recoveryToken: ID,
      url: 'https://example.test/file', [field]: 'untrusted'}));
  assert.throws(() => validateRequest({...base, command: 'recovery.resume', taskId: ID}));
  assert.throws(() => validateResponse({...base, ok: true, recoveryToken: 'broken'}, ID));
});
function dependencies(failure) {
  const order = [];
  const operation = name => async (...args) => {
    order.push([name, ...args]);
    if (failure === name || (Array.isArray(failure) && failure.includes(name))) throw new Error(name + ' failed');
    return name === 'prepare' ? ID : undefined;
  };
  return {order, deps: {pauseBrowser:operation('pause'), prepareNative:operation('prepare'), cancelBrowser:operation('cancel'), resumeBrowser:operation('restore'), cancelNative:operation('cancel-native'), resumeNative:operation('resume-native')}};
}
test('handoff cancels browser only after durable prepare ACK', async () => {
  const {order, deps} = dependencies(); assert.equal((await handoffDownload(deps)).status, 'transferred');
  assert.deepEqual(order.map(x => x[0]), ['pause','prepare','cancel','resume-native']);
  assert.deepEqual(order.at(-1), ['resume-native', ID]);
});
test('pause failure leaves original browser transfer alone', async () => {
  const {order,deps}=dependencies('pause'); assert.equal((await handoffDownload(deps)).status,'browser-restored');
  assert.deepEqual(order.map(x=>x[0]),['pause']);
});
test('prepare failure resumes browser without cancelling it', async () => {
  const {order,deps}=dependencies('prepare'); assert.equal((await handoffDownload(deps)).status,'browser-restored');
  assert.deepEqual(order.map(x=>x[0]),['pause','prepare','restore']);
});
test('cancel failure cancels paused native task and restores browser', async () => {
  const {order,deps}=dependencies('cancel'); assert.equal((await handoffDownload(deps)).status,'browser-restored');
  assert.deepEqual(order.map(x=>x[0]),['pause','prepare','cancel','cancel-native','restore']);
});
test('rollback failure is explicit and still attempts browser resume', async () => {
  const {order,deps}=dependencies(['cancel','cancel-native']); assert.equal((await handoffDownload(deps)).status,'recovery-required');
  assert.equal(order.at(-1)[0],'restore');
});
test('failed native resume retains durably prepared paused task', async () => {
  const {order,deps}=dependencies('resume-native'); const result=await handoffDownload(deps);
  assert.equal(result.status,'native-paused'); assert.equal(result.taskId,ID);
  assert.deepEqual(order.map(x=>x[0]),['pause','prepare','cancel','resume-native']);
});
test('failed browser restore produces recovery-required', async () => {
  const {deps}=dependencies(['prepare','restore']); assert.equal((await handoffDownload(deps)).status,'recovery-required');
});
test('invalid prepare ACK never cancels browser', async () => {
  const {order,deps}=dependencies(); deps.prepareNative=async()=> 'not-a-task';
  assert.equal((await handoffDownload(deps)).status,'browser-restored'); assert.deepEqual(order.map(x=>x[0]),['pause','restore']);
});
test('protocol rejects unsupported URLs, credentials and control characters', () => {
  for(const value of ['file:///a','javascript:alert(1)','blob:https://example.test/a','https://a:b@example.test/f','https://example.test/\nfile','https://example.test/'+ 'x'.repeat(32768)]) assert.throws(()=>safeHttpUrl(value));
  assert.equal(safeHttpUrl('https://example.test/file#fragment'),'https://example.test/file');
});
test('filenames never escape directories or retain Windows device names', () => {
  assert.equal(safeFilename('C:\\Downloads\\CON.txt'),'_CON.txt');
  assert.equal(safeFilename('../../file.zip'),'file.zip'); assert.equal(safeFilename('bad?.zip'),'bad_.zip');
  assert.throws(()=>safeFilename('file\r\nInjected'));
});
test('native request bounds and IDs are checked', () => {
  const good={version:1,requestId:ID,command:'prepare',url:'https://example.test/file'};
  assert.equal(validateRequest(good).command,'prepare');
  assert.throws(()=>validateRequest({...good,requestId:'../x'}));
  assert.throws(()=>validateRequest({...good,requestId:'ABCDEF0123456789ABCDEF0123456789'}));
  assert.throws(()=>validateRequest({version:1,requestId:ID,command:'resume',taskId:'ABCDEF0123456789ABCDEF0123456789'}));
  assert.throws(()=>validateRequest({...good,cookie:'x'.repeat(16385)}));
  assert.throws(()=>validateRequest({version:1,requestId:ID,command:'resume',taskId:'wrong'}));
  assert.match(newRequestId(),/^[a-f0-9]{32}$/);
});
test('native response matches protocol version, request and task ID', () => {
  const good={version:1,requestId:ID,ok:true,taskId:ID}; assert.equal(validateResponse(good,ID).ok,true);
  assert.throws(()=>validateResponse({...good,requestId:'different'},ID));
  assert.throws(()=>validateResponse({...good,version:2},ID));
  assert.throws(()=>validateResponse({...good,taskId:'../x'},ID));
});
test('discovery deduplicates, bounds and rejects unsafe URLs', () => {
  const values=[{url:'https://example.test/a',name:'a',kind:'media'},{url:'https://example.test/a#x'},{url:'javascript:x'}];
  assert.equal(normalizeCandidates(values).length,1);
  assert.equal(normalizeCandidates(Array.from({length:1000},(_,i)=>({url:`https://example.test/${i}`}))).length,200);
});
test('timeout rejects stalled native request and allows completed request', async () => {
  await assert.rejects(withTimeout(new Promise(()=>{}),5),/超时/);
  assert.equal(await withTimeout(Promise.resolve('ok'),100),'ok');
});
