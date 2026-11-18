import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {existsSync, mkdirSync, readFileSync, statSync, writeFileSync} from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {parseArgs} from 'node:util';

const {values} = parseArgs({options: {index: {type: 'string'}, browser: {type: 'string'}}});
assert(values.index, 'Supply the verified owned project index.');
const index = path.resolve(values.index);
assert(statSync(index).isFile());
const executable = values.browser || process.env.MASKPILOT_TEST_BROWSER;
assert(executable && existsSync(executable), 'Set MASKPILOT_TEST_BROWSER to an actual Chromium executable.');
const root = path.resolve('out/qa/site-project-browser-' + Date.now()); mkdirSync(root, {recursive: true});
const evidence = {result: 'running', index, requests: []};
class CDP {
  constructor(url) {
    this.socket = new WebSocket(url); this.id = 1; this.pending = new Map(); this.listeners = new Map();
    this.ready = new Promise((resolve, reject) => {
      this.socket.addEventListener('open', resolve, {once: true});
      this.socket.addEventListener('error', reject, {once: true});
    });
    this.socket.addEventListener('message', event => {
      const value = JSON.parse(String(event.data));
      if (value.id) {
        const pending = this.pending.get(value.id); if (!pending) return;
        clearTimeout(pending.timer); this.pending.delete(value.id);
        value.error ? pending.reject(new Error(JSON.stringify(value.error))) : pending.resolve(value.result);
      } else for (const listener of this.listeners.get(value.method) || []) listener(value.params);
    });
  }
  async call(method, params = {}) {
    await this.ready; const id = this.id++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {this.pending.delete(id); reject(new Error('CDP timeout: ' + method));}, 15000);
      this.pending.set(id, {resolve, reject, timer}); this.socket.send(JSON.stringify({id, method, params}));
    });
  }
  on(method, listener) {this.listeners.set(method, [...(this.listeners.get(method) || []), listener]);}
}
async function until(predicate, label, timeout = 15000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const value = await predicate(); if (value) return value;
    await new Promise(resolve => setTimeout(resolve, 40));
  }
  throw new Error('Owned offline project condition timed out: ' + label);
}
let browser, client, browserError;
try {
  const profile = path.join(root, 'profile');
  browser = spawn(path.resolve(executable), ['--headless=new', '--remote-debugging-port=0',
    `--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check', '--disable-extensions',
    '--disable-background-networking', '--disable-gpu', 'about:blank'], {windowsHide: true, stdio: 'ignore'});
  browser.on('error', error => {browserError = error;});
  const port = await until(() => {
    if (browserError) throw browserError;
    try {return readFileSync(path.join(profile, 'DevToolsActivePort'), 'utf8').split('\n')[0];}
    catch {return null;}
  }, 'debug port');
  const target = await until(async () => {
    const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    return targets.find(item => item.type === 'page');
  }, 'page target');
  client = new CDP(target.webSocketDebuggerUrl);
  client.on('Network.requestWillBeSent', event => evidence.requests.push(event.request.url));
  await client.call('Network.enable'); await client.call('Page.enable');
  await client.call('Emulation.setDeviceMetricsOverride', {width: 900, height: 650, deviceScaleFactor: 1, mobile: false});
  const navigation = await client.call('Page.navigate', {url: pathToFileURL(index).href});
  assert(!navigation.errorText, navigation.errorText);
  const evaluate = async expression => (await client.call('Runtime.evaluate', {
    expression, returnByValue: true, awaitPromise: true,
  })).result.value;
  await until(() => evaluate(`document.readyState==='complete' && document.querySelector('h1')?.textContent.startsWith('Fixture v') && document.querySelector('img')?.complete && document.querySelector('img')?.naturalWidth>0`), 'local HTML and image');
  assert.equal(await evaluate(`document.querySelector('img').decode().then(()=>true,()=>false)`), true);
  assert.equal(await evaluate(`getComputedStyle(document.body).color`), 'rgb(17, 34, 51)');
  assert.equal(await evaluate(`typeof window.projectSentinel`), 'undefined');
  assert.equal(await evaluate(`document.querySelectorAll('script,iframe,object,form,base').length`), 0);
  evidence.title = await evaluate(`document.title`);
  evidence.image = await evaluate(`({source:document.querySelector('img').currentSrc,width:document.querySelector('img').naturalWidth})`);
  await evaluate(`new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))`);
  const shot = await client.call('Page.captureScreenshot', {format: 'png', fromSurface: true, captureBeyondViewport: false,
    clip: {x: 0, y: 0, width: 900, height: 650, scale: 1}});
  writeFileSync(path.join(root, 'offline-project.png'), Buffer.from(shot.data, 'base64'));
  await evaluate(`Array.from(document.querySelectorAll('a')).find(a=>a.textContent==='Next').click()`);
  await until(() => evaluate(`document.readyState==='complete' && Array.from(document.querySelectorAll('a')).some(a=>a.textContent==='Home')`), 'offline second page');
  await evaluate(`Array.from(document.querySelectorAll('a')).find(a=>a.textContent==='Home').click()`);
  await until(() => evaluate(`document.readyState==='complete' && document.querySelector('h1')?.textContent.startsWith('Fixture v')`), 'offline return');
  assert.equal(evidence.requests.filter(url => /^https?:/i.test(url)).length, 0, JSON.stringify(evidence.requests));
  evidence.result = 'passed';
  console.log('PASS actual published project opens offline with decoded image/CSS imports/navigation, no source HTTP or active code');
} catch (error) {evidence.result = 'failed'; evidence.error = String(error.stack || error); throw error;}
finally {
  try {
    if (browser && browser.exitCode === null) {
      const closed = new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('Owned project browser did not stop')), 10000);
        browser.once('exit', () => {clearTimeout(timer); resolve();});
      });
      closed.catch(() => {});
      if (client) await client.call('Browser.close');
      else if (process.platform === 'win32') spawnSync('taskkill', ['/PID', String(browser.pid), '/T', '/F'], {windowsHide: true, timeout: 10000});
      else browser.kill();
      await closed;
    }
  } catch (error) {
    evidence.result = 'failed'; evidence.cleanupError = String(error); process.exitCode = 1;
    if (browser?.pid && browser.exitCode === null) {
      if (process.platform === 'win32') spawnSync('taskkill', ['/PID', String(browser.pid), '/T', '/F'], {windowsHide: true, timeout: 10000});
      else browser.kill('SIGKILL');
    }
  } finally {
    if (client) client.socket.close();
    writeFileSync(path.join(root, 'evidence.json'), JSON.stringify(evidence, null, 2));
    console.log('Evidence: ' + path.join(root, 'evidence.json'));
  }
}
