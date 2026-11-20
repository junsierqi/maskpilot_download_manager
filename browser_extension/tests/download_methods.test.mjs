import test from 'node:test';
import assert from 'node:assert/strict';
import {DownloadMethodLedger, observeDownloadMethods} from '../dist/download_methods.js';

const now = 1700000000000;
const url = 'https://example.test/export?owned=one';
const item = (options = {}) => ({url, startTime: new Date(now).toISOString(), incognito: false, ...options});
const event = (options = {}) => ({requestId: 'owned-1', url, method: 'GET', timeStamp: now,
  incognito: false, ...options});

for (const firefox of [false, true]) test(`only unique observed GET in exact namespace is eligible (${firefox})`, () => {
  const ledger = new DownloadMethodLedger(firefox, () => now);
  const context = firefox ? {cookieStoreId: 'firefox-default'} : {};
  const download = item(context);
  assert.equal(ledger.isGet(download), false);
  ledger.observe(event(context));
  assert.equal(ledger.isGet(download), false, 'A request without response metadata is not enough');
  ledger.observe(event(context), true);
  assert.equal(ledger.isGet(download), true);
  assert.equal(ledger.isGet(item({...context, incognito: true})), false);
  assert.equal(ledger.isGet(item({...context, byExtensionId: 'other-extension'})), false);
  assert.equal(ledger.isGet(item({...context, url: url + 'different'})), false);
  assert.equal(ledger.isGet(item({...context, startTime: 'invalid'})), false);
  if (firefox) assert.equal(ledger.isGet(item({cookieStoreId: 'firefox-container-1'})), false);
  ledger.observe(event({...context, requestId: 'owned-post', method: 'POST'}), true);
  assert.equal(ledger.isGet(download), false, 'Concurrent same-URL requests are ambiguous');
});

test('POST, unsupported, unknown context, redirect and worker restart fail closed', () => {
  for (const method of ['POST', 'PUT', 'HEAD', 'get', '?']) {
    const ledger = new DownloadMethodLedger(false, () => now);
    ledger.observe(event({method}), true);
    assert.equal(ledger.isGet(item()), false);
  }
  const ledger = new DownloadMethodLedger(false, () => now);
  const key = ledger.observe(event({incognito: undefined}), true);
  assert.equal(ledger.isGet(item()), false);
  ledger.bindContext(key, {incognito: false});
  assert.equal(ledger.isGet(item()), true);
  assert.equal(new DownloadMethodLedger(false, () => now).isGet(item()), false);
  const redirect = new DownloadMethodLedger(false, () => now);
  redirect.observe(event({method: 'POST', url: 'https://example.test/submit'}), true);
  redirect.observe(event(), true);
  assert.equal(redirect.isGet(item({url: 'https://example.test/submit', finalUrl: url})), true);
  const different = new DownloadMethodLedger(true, () => now);
  different.observe(event({method: 'POST', cookieStoreId: 'firefox-container-1'}), true);
  different.observe(event({cookieStoreId: 'firefox-default'}), true);
  assert.equal(different.isGet(item({cookieStoreId: 'firefox-default'})), true);
});

test('metadata is bounded and expired, rollback-clock or unrelated old responses cannot match', () => {
  let clock = now;
  const ledger = new DownloadMethodLedger(false, () => clock);
  for (let n = 0; n < 300; ++n) ledger.observe(event({requestId: 'owned-' + n, url: url + n}), true);
  assert.equal(ledger.size, 256);
  assert.equal(ledger.isGet(item({url: url + '0'})), false);
  assert.equal(ledger.isGet(item({url: url + '299'})), true);
  assert.equal(ledger.isGet(item({url: url + '299', startTime: new Date(now - 5000).toISOString()})), false);
  clock += 10 * 60 * 1000 + 1;
  assert.equal(ledger.size, 0);
  assert.equal(ledger.isGet(item({url: url + '299'})), false);
  ledger.observe(event({timeStamp: clock}), true);
  --clock;
  assert.equal(ledger.size, 0);
  for (const bad of [{url: 'file:///private'}, {url: 'https://u:p@example.test/'}, {requestId: ''},
    {method: ''}, {timeStamp: NaN}, {timeStamp: clock + 6000}]) {
    ledger.observe(event(bad), true);
    assert.equal(ledger.size, 0);
  }
});

test('observer requests only metadata, resolves trusted Chrome tab and bounds pending lookups', async () => {
  const callbacks = {};
  let lookups = 0;
  const api = {webRequest: Object.fromEntries(['onBeforeRequest', 'onHeadersReceived'].map(name => [name,
    {addListener(...args) {
      assert.equal(args.length, 2, 'No requestBody/headers/blocking options');
      assert.deepEqual(args[1], {urls: ['http://*/*', 'https://*/*']});
      callbacks[name] = args[0];
    }}])), tabs: {get: async id => { ++lookups; assert.equal(id, 7); return {incognito: false}; }}};
  const isGet = observeDownloadMethods(api, false);
  const details = event({incognito: undefined, timeStamp: Date.now(), tabId: 7});
  Object.defineProperty(details, 'requestBody', {get() { assert.fail('Request body was accessed'); }});
  Object.defineProperty(details, 'responseHeaders', {get() { assert.fail('Response headers were accessed'); }});
  callbacks.onBeforeRequest(details);
  callbacks.onHeadersReceived(details);
  assert.equal(await isGet(item({startTime: new Date().toISOString()})), true);
  assert(lookups > 0);
  api.tabs.get = () => { ++lookups; return new Promise(() => {}); };
  const before = lookups;
  for (let n = 0; n < 100; ++n)
    callbacks.onBeforeRequest({...event({timeStamp: Date.now(), incognito: undefined, requestId: 'pending-' + n}), tabId: 7});
  assert.equal(lookups - before, 64);
  assert.equal(await isGet(item({startTime: new Date().toISOString()})), false);
});


test('missing observer capability preserves browser ownership without breaking manual extension features', async () => {
  assert.equal(await observeDownloadMethods({}, false)(item()), false);
});


test('incomplete Firefox event namespace resolves trusted tab and later incomplete metadata cannot erase it', async () => {
  const callbacks = {};
  const api = {webRequest: Object.fromEntries(['onBeforeRequest', 'onHeadersReceived'].map(name => [name,
    {addListener(callback) { callbacks[name] = callback; }}])),
    tabs: {get: async () => ({incognito: false, cookieStoreId: 'firefox-default'})}};
  const isGet = observeDownloadMethods(api, true);
  const details = event({timeStamp: Date.now(), tabId: 7, cookieStoreId: undefined});
  callbacks.onBeforeRequest(details);
  await Promise.resolve();
  callbacks.onHeadersReceived({...details, tabId: -1});
  assert.equal(await isGet(item({startTime: new Date().toISOString(), cookieStoreId: 'firefox-default'})), true);
});
