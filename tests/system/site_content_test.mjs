import assert from 'node:assert/strict';
import {spawn, spawnSync} from 'node:child_process';
import {createServer} from 'node:http';
import {mkdirSync, readFileSync, writeFileSync} from 'node:fs';
import path from 'node:path';
import {pathToFileURL} from 'node:url';
import {parseArgs} from 'node:util';
import {deflateSync} from 'node:zlib';

const {values} = parseArgs({options: {
  'build-directory': {type: 'string', default: 'out/build/windows-msvc-completion'},
  browser: {type: 'string'},
}});
assert(values.browser, 'Supply --browser with a real Chromium executable.');
const build = path.resolve(values['build-directory']);
const root = path.resolve('out/qa/site-content-' + Date.now());
mkdirSync(root, {recursive: true});
const evidence = {result: 'running', groups: [], root};
const contacted = [];
const server = createServer((request, response) => {
  contacted.push(request.url);
  response.writeHead(500); response.end('Unexpected parser/browser request');
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
const secret = 'owned-parser-secret-must-never-be-read';
const secretPath = path.join(root, 'owned-secret.txt');
writeFileSync(secretPath, secret);
const mappings = {
  [`${origin}/site/theme`]: 'theme.css',
  [`${origin}/site/base`]: 'base.css',
  [`${origin}/site/logo`]: 'logo.png',
  [`${origin}/site/small`]: 'logo.png',
  [`${origin}/site/large`]: 'logo.png',
  [`${origin}/site/next`]: 'next.html',
};
function processContent(url, contentType, body) {
  const result = spawnSync(path.join(build, 'tests/maskpilot_site_content_probe.exe'), [], {
    input: JSON.stringify({url, contentType, bodyBase64: Buffer.from(body).toString('base64'), mappings}),
    encoding: 'utf8', maxBuffer: 32 * 1024 * 1024, timeout: 15000, windowsHide: true,
  });
  assert.equal(result.status, 0, result.stderr);
  return JSON.parse(result.stdout);
}
function imageFixture() {
  const dimension = 64;
  const crc32 = data => {
    let crc = 0xffffffff;
    for (const value of data) {
      crc ^= value;
      for (let bit = 0; bit < 8; ++bit) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return (crc ^ 0xffffffff) >>> 0;
  };
  const chunk = (name, data) => {
    const content = Buffer.concat([Buffer.from(name), data]);
    const length = Buffer.alloc(4), crc = Buffer.alloc(4);
    length.writeUInt32BE(data.length); crc.writeUInt32BE(crc32(content));
    return Buffer.concat([length, content, crc]);
  };
  const header = Buffer.alloc(13);
  header.writeUInt32BE(dimension, 0); header.writeUInt32BE(dimension, 4); header[8] = 8; header[9] = 6;
  const rows = Buffer.alloc(dimension * (1 + dimension * 4));
  for (let y = 0; y < dimension; ++y) for (let x = 0; x < dimension; ++x) {
    const at = y * (1 + dimension * 4) + 1 + x * 4;
    rows.set([25, 65, 85, 255], at);
  }
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', header),
    chunk('IDAT', deflateSync(rows)), chunk('IEND', Buffer.alloc(0))]);
}
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
        const pending = this.pending.get(value.id);
        if (!pending) return;
        clearTimeout(pending.timer); this.pending.delete(value.id);
        value.error ? pending.reject(new Error(JSON.stringify(value.error))) : pending.resolve(value.result);
      } else {
        for (const listener of this.listeners.get(value.method) || []) listener(value.params);
      }
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
async function until(predicate, timeout = 15000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const value = await predicate(); if (value) return value;
    await new Promise(resolve => setTimeout(resolve, 40));
  }
  throw new Error('Owned offline browser condition timed out.');
}
let browser, client;
try {
  const html = `<!DOCTYPE html SYSTEM "${origin}/external.dtd"><html><head>
    <base href="/site/"><title>Owned offline page</title><link rel="stylesheet" href="theme">
    <meta http-equiv="refresh" content="0;url=${origin}/refresh"></head>
    <body onload="window.activeSentinel=1"><h1>Offline content</h1>
    <img id="owned-image" src="logo" srcset="small 1x, large 2x">
    <a id="next" href="next#chapter">Next offline page</a>
    <script>window.activeSentinel=1;fetch('${origin}/active');</script>
    <style>p::before { content:"\\3c /style\\3e \\3c img id='escaped-style-injection' src='file:///C:/private.png'\\3e "; }</style>
    <iframe src="${origin}/frame"></iframe><object data="${origin}/object"></object>
    <svg onload="fetch('${origin}/svg')"></svg>
    <form action="${origin}/submit"><input name="private"></form>
    <img src="http://foreign.invalid/pixel"><p>Public static fixture</p></body></html>`;
  const result = processContent(`${origin}/docs/index`, 'text/html; charset=UTF-8', html);
  const theme = processContent(`${origin}/site/theme`, 'text/css; charset=UTF-8',
    `@import "base"; body { background-image: u\\72l(logo); } .bad { background:url(http://foreign.invalid/pixel); }`);
  const base = processContent(`${origin}/site/base`, 'text/css', 'body { background-color: rgb(12,34,56); color: white; }');
  const next = processContent(`${origin}/site/next`, 'text/html; charset=UTF-8',
    '<html><head><title>Next</title></head><body><h1 id="chapter">Second offline page</h1></body></html>');
  const localDtd = processContent(`${origin}/local`, 'text/html',
    `<!DOCTYPE html SYSTEM "${pathToFileURL(secretPath).href}"><html><body>Safe local input</body></html>`);
  assert(!localDtd.body.includes(secret));
  assert.equal(contacted.length, 0, 'Parser attempted external resource IO.');
  for (const forbidden of ['<script', '<iframe', '<object', '<svg', '<form', '<base', 'onload', 'activeSentinel', 'foreign.invalid'])
    assert(!result.body.includes(forbidden), forbidden);
  assert(result.references.some(value => value.url === `${origin}/site/theme`));
  writeFileSync(path.join(root, 'index.html'), result.body);
  writeFileSync(path.join(root, 'next.html'), next.body);
  writeFileSync(path.join(root, 'theme.css'), theme.body);
  writeFileSync(path.join(root, 'base.css'), base.body);
  writeFileSync(path.join(root, 'logo.png'), imageFixture());
  evidence.groups.push('maintained parser has no remote/local DTD IO and strips active/foreign content');
  console.log('PASS production parser bounded HTML/CSS references, base/srcset/import rewrites and denied external loaders');
  await new Promise(resolve => server.close(resolve));
  evidence.sourcesStoppedBeforeBrowser = true;
  const profile = path.join(root, 'profile');
  browser = spawn(path.resolve(values.browser), ['--headless=new', '--remote-debugging-port=0',
    `--user-data-dir=${profile}`, '--no-first-run', '--no-default-browser-check', '--disable-extensions',
    '--disable-background-networking', '--disable-gpu', 'about:blank'], {windowsHide: true, stdio: 'ignore'});
  const portFile = path.join(profile, 'DevToolsActivePort');
  const port = await until(() => {try {return readFileSync(portFile, 'utf8').split('\n')[0];} catch {return null;}});
  const target = await until(async () => {
    const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    return targets.find(value => value.type === 'page');
  });
  client = new CDP(target.webSocketDebuggerUrl);
  const requests = [];
  client.on('Network.requestWillBeSent', event => requests.push(event.request.url));
  await client.call('Network.enable'); await client.call('Page.enable');
  await client.call('Emulation.setDeviceMetricsOverride', {width: 800, height: 600, deviceScaleFactor: 2, mobile: false});
  await client.call('Page.navigate', {url: pathToFileURL(path.join(root, 'index.html')).href});
  const evaluate = async expression => (await client.call('Runtime.evaluate', {
    expression, returnByValue: true, awaitPromise: true,
  })).result.value;
  try {
    await until(() => evaluate(`document.readyState==='complete' && document.querySelector('#owned-image')?.naturalWidth===32`));
  } catch (error) {
    evidence.pageState = await evaluate(`({url:location.href,ready:document.readyState,dpr:devicePixelRatio,
      image:document.querySelector('#owned-image') && {complete:document.querySelector('#owned-image').complete,
      width:document.querySelector('#owned-image').naturalWidth,src:document.querySelector('#owned-image').currentSrc},
      background:getComputedStyle(document.body).backgroundColor})`);
    evidence.requests = requests;
    throw error;
  }
  assert.equal(await evaluate(`document.querySelector('#owned-image').decode().then(()=>true,()=>false)`), true);
  evidence.image = await evaluate(`({dpr:devicePixelRatio,naturalWidth:document.querySelector('#owned-image').naturalWidth})`);
  assert.equal(await evaluate('getComputedStyle(document.body).backgroundColor'), 'rgb(12, 34, 56)');
  assert.equal(await evaluate('typeof window.activeSentinel'), 'undefined');
  assert.equal(await evaluate(`document.querySelectorAll('script,iframe,object,svg,form,base').length`), 0);
  assert.equal(await evaluate(`document.querySelectorAll('#escaped-style-injection').length`), 0);
  // Keep the functional assertions at 2x; capture a 1x viewport independently of host display scaling.
  await client.call('Emulation.setDeviceMetricsOverride', {width: 800, height: 600, deviceScaleFactor: 1, mobile: false});
  await evaluate(`new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))`);
  const shot = await client.call('Page.captureScreenshot', {format: 'png', fromSurface: true,
    captureBeyondViewport: false, clip: {x: 0, y: 0, width: 800, height: 600, scale: 1}});
  writeFileSync(path.join(root, 'offline-render.png'), Buffer.from(shot.data, 'base64'));
  await evaluate(`document.querySelector('#next').click()`);
  await until(() => evaluate(`location.pathname.endsWith('/next.html') && document.querySelector('h1')?.textContent==='Second offline page'`));
  assert.equal(await evaluate('location.hash'), '#chapter');
  assert.equal(requests.filter(url => /^https?:/i.test(url)).length, 0, JSON.stringify(requests));
  assert.equal(contacted.length, 0);
  evidence.groups.push('actual file browser loads local CSS import/image and offline navigation with sources stopped');
  evidence.groups.push('zero HTTP requests and no active sentinel execution');
  evidence.requests = requests;
  console.log('PASS actual Chromium file:// CSS import/image/navigation with origin OFF, zero remote requests and no active code');
  evidence.result = 'passed';
} catch (error) {
  evidence.result = 'failed'; evidence.error = String(error.stack || error); throw error;
} finally {
  try {
    if (browser && browser.exitCode === null) {
      const closed = new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error('Owned browser did not stop')), 10000);
        browser.once('exit', () => {clearTimeout(timer); resolve();});
      });
      closed.catch(() => {}); // A close-command failure must not leave an unhandled deadline promise.
      if (client) await client.call('Browser.close');
      else if (process.platform === 'win32')
        spawnSync('taskkill', ['/PID', String(browser.pid), '/T', '/F'], {windowsHide: true, timeout: 10000});
      else browser.kill();
      await closed;
    }
  } catch (error) {
    evidence.result = 'failed'; evidence.cleanupError = String(error.stack || error); process.exitCode = 1;
    if (browser?.pid && browser.exitCode === null) {
      if (process.platform === 'win32')
        spawnSync('taskkill', ['/PID', String(browser.pid), '/T', '/F'], {windowsHide: true, timeout: 10000});
      else browser.kill('SIGKILL');
    }
  } finally {
    if (client) client.socket.close();
    if (server.listening) await new Promise(resolve => server.close(resolve));
    writeFileSync(path.join(root, 'evidence.json'), JSON.stringify(evidence, null, 2));
    console.log('Evidence: ' + path.join(root, 'evidence.json'));
  }
}
