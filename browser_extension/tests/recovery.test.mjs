import {test} from 'node:test';
import assert from 'node:assert/strict';
import {BrowserRecoveryController} from '../dist/browser_recovery.js';

const TASK = 'a'.repeat(32), TOKEN = 'b'.repeat(32), SECRET = 'fresh-cookie-never-store';
function fixture({firefox = false} = {}) {
  const state = {now: 1_800_000_000_000, session: {}, calls: [], cookies: true, nativeSaved: false,
    nativeSaves: 0, failApply: false, lostAck: false, failCancel: false, failResume: false, failAppliedStore: false,
    invalidated: false, observedGet: true, pausedState: 'Paused', nextTab: 2};
  const copy = value => structuredClone(value);
  const tabs = new Map([[1, {id: 1, windowId: 1, url: 'https://example.test/start', incognito: false,
    ...(firefox ? {cookieStoreId: 'firefox-default'} : {})}]]);
  const downloads = new Map([[7, {id: 7, url: 'https://example.test/fresh?signature=one',
    finalUrl: 'https://example.test/fresh?signature=one', referrer: 'https://example.test/page',
    startTime: new Date(state.now + 20).toISOString(), filename: 'C:/browser/fresh.bin',
    state: 'in_progress', paused: false, incognito: false,
    ...(firefox ? {cookieStoreId: 'firefox-default'} : {})}]]);
  const api = {
    runtime: {id: 'owned-test-extension'},
    storage: {
      session: {
        setAccessLevel: async value => { assert.equal(value.accessLevel, 'TRUSTED_CONTEXTS'); },
        get: async () => copy(state.session),
        set: async value => {
          state.calls.push('store:' + value.browserRecovery.phase);
          if (state.failAppliedStore && value.browserRecovery.phase === 'applied') throw new Error('memory-store failure');
          state.session = copy(value);
        },
        remove: async () => { state.calls.push('clear-session'); state.session = {}; }
      },
      local: {set: async () => { assert.fail('Recovery must not persist browser local storage'); }}
    },
    tabs: {
      query: async () => [copy(tabs.get(1))],
      get: async id => { assert(tabs.has(id), 'Owned source tab missing'); return copy(tabs.get(id)); },
      create: async details => {
        state.calls.push('open-source');
        if (firefox) assert.equal(details.cookieStoreId, 'firefox-default');
        const tab = {id: state.nextTab++, windowId: 1, url: details.url, incognito: false,
          ...(firefox ? {cookieStoreId: details.cookieStoreId} : {})};
        tabs.set(tab.id, tab); return copy(tab);
      }
    },
    cookies: {getAll: async query => {
      state.calls.push('read-cookie:' + query.url);
      assert.deepEqual(query.partitionKey, {});
      if (firefox) assert.equal(query.storeId, 'firefox-default');
      return [{name: 'session', value: SECRET}];
    }},
    downloads: {
      search: async query => {
        const values = [...downloads.values()].filter(item => query.id === undefined || item.id === query.id);
        return copy(values);
      },
      pause: async id => {
        state.calls.push('pause-browser:' + id);
        downloads.get(id).paused = true;
        if (firefox) downloads.get(id).state = 'interrupted';
      },
      cancel: async id => {
        assert(state.nativeSaved, 'Browser cancelled before durable original-task acknowledgement');
        state.calls.push('cancel-browser:' + id);
        if (state.failCancel) throw new Error('Browser cancel failed');
        Object.assign(downloads.get(id), {paused: false, state: 'interrupted', error: 'USER_CANCELED'});
      },
      resume: async id => {
        state.calls.push('restore-browser:' + id);
        Object.assign(downloads.get(id), {paused: false, state: 'in_progress'});
      }
    }
  };
  const deps = {api, firefox, isGetDownload: async () => state.observedGet, now: () => state.now, includeCookies: async () => state.cookies,
    userAgent: () => 'owned-UA', send: async (command, fields = {}) => {
      state.calls.push('native:' + command);
      assert(!['add', 'prepare', 'cancel', 'resume', 'remove'].includes(command), 'Old task cannot use new-task rollback or plain-ID resume');
      if (command === 'pause') return {ok: true, taskId: TASK};
      if (command === 'list') return {ok: true, tasks: [{id: TASK, state: state.pausedState, canRecover: true}]};
      if (command === 'recovery.begin') return {ok: true, taskId: TASK, recoveryToken: TOKEN,
        name: 'original.bin', pageUrl: 'https://example.test/page', expiresAt: state.now / 1000 + 600};
      if (command === 'recovery.apply') {
        assert.equal(fields.recoveryToken, TOKEN);
        assert.equal(fields.cookie, 'session=' + SECRET);
        assert(!('destination' in fields) && !('proxy' in fields) && !('taskId' in fields));
        if (state.failApply) throw new Error('Native save rejected');
        if (!state.nativeSaved) ++state.nativeSaves;
        state.nativeSaved = true;
        if (state.lostAck) { state.lostAck = false; throw new Error('Durable acknowledgement lost'); }
        state.material = copy(fields);
        return {ok: true, taskId: TASK};
      }
      if (command === 'recovery.resume') {
        assert(state.nativeSaved);
        assert.equal(fields.recoveryToken, TOKEN);
        if (state.failResume || state.invalidated) throw new Error('Checked resume rejected');
        return {ok: true, taskId: TASK};
      }
      if (command === 'recovery.cancel') return {ok: true};
      assert.fail('Unexpected native command ' + command);
    }};
  return {state, tabs, downloads, api, deps, controller: new BrowserRecoveryController(deps)};
}

for (const firefox of [false, true]) test(`selected record uses exact URL/container and durable ACK before cancellation (${firefox ? 'Firefox' : 'Chromium'})`, async () => {
  const {state, downloads, controller} = fixture({firefox});
  downloads.set(8, {...downloads.get(7), id: 8, url: 'https://example.test/unrelated', finalUrl: 'https://example.test/unrelated'});
  const view = await controller.begin(TASK);
  assert(view.active && !('token' in view));
  assert(await controller.hasPending());
  assert.equal((await controller.downloads()).length, 2);
  await controller.useDownload(7);
  assert.equal(state.material.url, 'https://example.test/fresh?signature=one');
  assert.equal(downloads.get(8).paused, false);
  assert(!state.calls.some(call => call.includes('browser:8')));
  assert(state.calls.indexOf('native:recovery.apply') < state.calls.indexOf('cancel-browser:7'));
  assert(state.calls.indexOf('store:applied') < state.calls.indexOf('cancel-browser:7'));
  assert(state.calls.indexOf('cancel-browser:7') < state.calls.indexOf('native:recovery.resume'));
  assert(!(await controller.hasPending()));
});

test('apply failure restores only a flow-paused browser copy and never cancels original task', async () => {
  for (const previouslyPaused of [false, true]) {
    const {state, downloads, controller} = fixture();
    downloads.get(7).paused = previouslyPaused;
    await controller.begin(TASK); state.failApply = true;
    await assert.rejects(controller.useDownload(7), /更新未得到确认/);
    assert(!state.calls.includes('cancel-browser:7'));
    assert(!state.calls.includes('native:recovery.resume'));
    assert.equal(state.calls.includes('restore-browser:7'), !previouslyPaused);
    assert.equal(downloads.get(7).paused, previouslyPaused);
  }
});

test('cancel failure keeps applied phase across worker/controller replacement; explicit continue uses checked token', async () => {
  const {state, deps, controller} = fixture();
  await controller.begin(TASK); state.failCancel = true;
  assert.match((await controller.useDownload(7)).message, /未能取消/);
  assert(!state.calls.includes('native:recovery.resume'));
  assert.equal(state.session.browserRecovery.phase, 'applied');
  assert(!JSON.stringify(state.session).includes(SECRET));
  const restarted = new BrowserRecoveryController(deps);
  assert.equal((await restarted.status()).phase, 'applied');
  state.failCancel = false;
  await restarted.continue();
  assert.equal(state.calls.filter(value => value === 'native:recovery.resume').length, 1);
  assert.deepEqual(state.session, {});
});

test('lost apply ACK and failed phase store both retain browser copy; retry never assumes native start', async () => {
  for (const failure of ['lostAck', 'failAppliedStore']) {
    const {state, deps, controller} = fixture();
    await controller.begin(TASK); state[failure] = true;
    await assert.rejects(controller.useDownload(7));
    assert(state.nativeSaved);
    assert(!state.calls.includes('cancel-browser:7'));
    assert(!state.calls.includes('native:recovery.resume'));
    state[failure] = false;
    await new BrowserRecoveryController(deps).useDownload(7);
    assert.equal(state.nativeSaves, 1);
    assert.deepEqual(state.session, {});
  }
});

test('changed source container or partitioned cookie rejects before native apply and browser cancellation', async () => {
  for (const kind of ['container', 'partition']) {
    const {state, api, tabs, controller} = fixture({firefox: true});
    await controller.begin(TASK);
    api.cookies.getAll = async () => {
      if (kind === 'container') tabs.get(2).cookieStoreId = 'firefox-container-other';
      return [{name: 'session', value: SECRET, ...(kind === 'partition' ? {partitionKey: {topLevelSite: 'https://other.test'}} : {})}];
    };
    await assert.rejects(controller.useDownload(7));
    assert(!state.calls.includes('native:recovery.apply'));
    assert(!state.calls.includes('cancel-browser:7'));
    assert(state.calls.includes('restore-browser:7'));
  }
});

test('private/missing Firefox context fails before original native task mutation', async () => {
  for (const kind of ['private', 'missing']) {
    const {state, tabs, controller} = fixture({firefox: true});
    if (kind === 'private') tabs.get(1).incognito = true;
    else delete tabs.get(1).cookieStoreId;
    await assert.rejects(controller.begin(TASK), /无法安全绑定/);
    assert(!state.calls.some(value => value.startsWith('native:')));
  }
});

test('expired or replaced task cannot be started by unbound fallback; no completed browser file is deleted', async () => {
  const {state, downloads, controller} = fixture();
  await controller.begin(TASK); state.failCancel = true;
  await controller.useDownload(7); state.failCancel = false; state.invalidated = true;
  const result = await controller.continue();
  assert.match(result.message, /确认/);
  assert.equal(state.session.browserRecovery.phase, 'applied');
  downloads.get(7).state = 'complete'; downloads.get(7).paused = false;
  const cancels = state.calls.filter(value => value === 'cancel-browser:7').length;
  await assert.rejects(controller.continue(), /副本已完成/);
  assert.equal(state.calls.filter(value => value === 'cancel-browser:7').length, cancels);
  state.now += 601_000;
  assert.equal((await controller.status()).expired, true);
  await assert.rejects(controller.continue(), /失效/);
  await controller.cancel();
  assert.deepEqual(state.session, {});
});

test('manual link update stays paused and uses only supported current container cookies', async () => {
  const {state, controller} = fixture({firefox: true});
  await controller.begin(TASK);
  assert.match((await controller.useUrl('https://example.test/new-direct')).message, /保持暂停/);
  assert.equal(state.material.url, 'https://example.test/new-direct');
  assert(!state.calls.includes('native:recovery.resume'));
  assert(!state.calls.includes('cancel-browser:7'));
  assert.deepEqual(state.session, {});
});

test('corrupt session can be explicitly cleared without trusting its task/browser fields', async () => {
  const {state, controller} = fixture();
  state.session.browserRecovery = {token: 'not-a-token', downloadId: 999, taskId: TASK};
  await assert.rejects(controller.status(), /会话无效/);
  await controller.cancel();
  assert.deepEqual(state.session, {});
  assert(!state.calls.some(value => value.startsWith('native:') || value.includes('browser:')));
});

test('new recovery cannot silently replace an unfinished selection session', async () => {
  const {state, controller} = fixture();
  await controller.begin(TASK);
  const before = [...state.calls];
  await assert.rejects(controller.begin('c'.repeat(32)), /先完成或取消/);
  assert.deepEqual(state.calls, before);
  await controller.cancel();
  assert(state.calls.includes('native:recovery.cancel'));
});


test('POST or unknown selected records are left browser-owned before pause or native mutation', async () => {
  const {state, controller} = fixture({firefox: true});
  await controller.begin(TASK);
  state.observedGet = false;
  assert.deepEqual(await controller.downloads(), []);
  const before = state.calls.length;
  await assert.rejects(controller.useDownload(7), /GET/);
  assert(!state.calls.slice(before).some(value => value.startsWith('pause-browser:') ||
    value === 'native:recovery.apply' || value.startsWith('cancel-browser:')));
  assert.equal(state.nativeSaved, false);
});

test('applied receipt and rollback remain usable after method memory is lost', async () => {
  const applied = fixture();
  await applied.controller.begin(TASK);
  applied.state.failResume = true;
  await applied.controller.useDownload(7);
  assert.equal(applied.state.session.browserRecovery.phase, 'applied');
  applied.state.observedGet = false;
  applied.state.failResume = false;
  await new BrowserRecoveryController(applied.deps).continue();
  assert.equal(applied.state.session.browserRecovery, undefined);

  const rollback = fixture();
  await rollback.controller.begin(TASK);
  rollback.state.failApply = true;
  const send = rollback.deps.send;
  rollback.deps.send = async (...args) => {
    if (args[0] === 'recovery.apply') rollback.state.observedGet = false;
    return send(...args);
  };
  await assert.rejects(rollback.controller.useDownload(7));
  assert.equal(rollback.downloads.get(7).paused, false);
  assert(rollback.state.calls.includes('restore-browser:7'));
});


test('a full page of unknown methods uses one bounded observation window and preserves every browser record', async () => {
  const {controller, deps, downloads, state} = fixture();
  await controller.begin(TASK);
  for (let n = 8; n < 27; ++n) downloads.set(n, {...downloads.get(7), id: n});
  const began = Date.now();
  deps.isGetDownload = async () => { await new Promise(resolve => setTimeout(resolve, 40)); return false; };
  const before = state.calls.length;
  assert.deepEqual(await controller.downloads(), []);
  assert(Date.now() - began < 500, 'Unknown records caused serial observation delays');
  assert(!state.calls.slice(before).some(value => value.startsWith('pause-browser:') || value.startsWith('cancel-browser:')));
});
