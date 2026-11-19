import {HOST_NAME, handoffDownload, isRecord, newRequestId, safeHttpUrl, validateRequest,
  validateResponse, withTimeout, type Command, type NativeRequest, type NativeResponse} from './protocol.js';
import {browserApi as api, isFirefox} from './browser_api.js';
import {cookieContext, cookieHeader, type CookieContext} from './browser_context.js';
import {PanelAuthority, type PageContext, type PanelModel} from './panel_authority.js';
import {BrowserRecoveryController} from './browser_recovery.js';

const inFlight = new Set<number>();
const authority = new PanelAuthority();
interface Preferences {autoCapture: boolean; includeCookies: boolean; pagePanel: boolean;}
interface PageState {scan: number; context?: PageContext; cookies?: CookieContext;}
const pages = new Map<number, PageState>();
async function preferences(): Promise<Preferences> {
  const saved = await api.storage.local.get({autoCapture: false, includeCookies: false, pagePanel: false});
  return {autoCapture: saved.autoCapture === true, includeCookies: saved.includeCookies === true,
    pagePanel: saved.pagePanel === true};
}
async function native(request: NativeRequest, authorize?: () => void): Promise<NativeResponse> {
  const validated = validateRequest(request);
  if (validated.cookie && !(await preferences()).includeCookies)
    throw new Error('登录信息转发已关闭，请重新发送请求。');
  authorize?.();
  const response = validateResponse(await withTimeout(api.runtime.sendNativeMessage(HOST_NAME, validated)), validated.requestId);
  if (!response.ok) throw new Error(response.error || '桌面程序未接受请求。');
  return response;
}
async function command(command: Command, fields: Partial<NativeRequest> = {}): Promise<NativeResponse> {
  return native({...fields, version: 1, requestId: newRequestId(), command});
}
const recovery = new BrowserRecoveryController({api, firefox: isFirefox, send: command,
  includeCookies: async () => (await preferences()).includeCookies, userAgent: () => navigator.userAgent});
async function addFields(url: string, context: CookieContext, filename?: string, referrer?: string,
  media = false): Promise<Partial<NativeRequest>> {
  const selected = safeHttpUrl(url);
  let safeReferrer: string | undefined;
  try { if (referrer) safeReferrer = safeHttpUrl(referrer); } catch { /* Internal page. */ }
  const cookie = await cookieHeader(selected, context, (await preferences()).includeCookies,
    details => api.cookies.getAll(details));
  return {url: selected, filename, referrer: safeReferrer, cookie, userAgent: navigator.userAgent, media};
}
async function report(error: string): Promise<void> {
  await api.storage.local.set({lastError: error.slice(0, 4096)});
  await api.action.setBadgeText({text: '!'});
  await api.action.setBadgeBackgroundColor({color: '#bd453b'});
}
async function clearError(): Promise<void> {
  await api.storage.local.remove('lastError');
  await api.action.setBadgeText({text: ''});
}
function invalidate(tabId: number): void { pages.delete(tabId); authority.invalidate(tabId); }
function stateFor(tabId: number): PageState {
  let state = pages.get(tabId);
  if (!state) {
    if (pages.size >= 64) invalidate(pages.keys().next().value!);
    state = {scan: 0}; pages.set(tabId, state);
  }
  return state;
}
async function assertPage(state: PageState): Promise<void> {
  const context = state.context;
  if (!context || pages.get(context.tabId) !== state) throw new Error('页面已更改，请重新查找资源。');
  const tab = await api.tabs.get(context.tabId);
  const cookies = cookieContext(tab, isFirefox);
  if (pages.get(context.tabId) !== state || safeHttpUrl(tab.url) !== context.url ||
      cookies.storeId !== state.cookies?.storeId || cookies.incognito !== state.cookies?.incognito)
    throw new Error('页面或 Cookie 容器已更改，请重新查找资源。');
  const documents = await api.scripting.executeScript({target: {tabId: context.tabId, frameIds: [0]},
    func: () => (globalThis as typeof globalThis & {maskpilotDocument?: string}).maskpilotDocument});
  if (pages.get(context.tabId) !== state || documents[0]?.result !== context.documentId)
    throw new Error('页面文档已更改，请重新查找资源。');
}
async function scan(tabId: number): Promise<{state: PageState; model: PanelModel}> {
  const state = stateFor(tabId), generation = ++state.scan;
  const tab = await api.tabs.get(tabId), url = safeHttpUrl(tab.url);
  const results = await api.scripting.executeScript({target: {tabId, frameIds: [0]}, files: ['content.js']});
  const result: unknown = results[0]?.result;
  if (pages.get(tabId) !== state || state.scan !== generation || !isRecord(result) ||
      typeof result.document !== 'string' || !/^[a-f0-9]{32}$/.test(result.document))
    throw new Error('页面已更改或资源不可读取。');
  state.context = {tabId, url, documentId: result.document};
  state.cookies = cookieContext(tab, isFirefox);
  await assertPage(state);
  const model = authority.open(state.context, result.items);
  return {state, model};
}
async function showPanel(tabId: number): Promise<void> {
  if (!(await preferences()).pagePanel) return;
  const {state, model} = await scan(tabId);
  if (!(await preferences()).pagePanel) return;
  await assertPage(state);
  await api.scripting.executeScript({target: {tabId, frameIds: [0]}, files: ['page_panel.js']});
  await api.tabs.sendMessage(tabId, {kind: 'panel-model', model}, {frameId: 0});
}
async function submit(state: PageState, token: string, ids: unknown, panel: boolean) {
  await assertPage(state);
  return authority.submit(state.context!, token, ids, async (candidate, requestId) => {
    if (panel && !(await preferences()).pagePanel) throw new Error('页面面板已关闭。');
    const fields = await addFields(candidate.url, state.cookies!, undefined, state.context!.url, candidate.kind === 'media');
    await assertPage(state);
    if (panel && !(await preferences()).pagePanel) throw new Error('页面面板已关闭。');
    authority.assertCurrent(state.context!, token);
    const response = await native({...fields, version: 1, command: 'add', requestId}, () => {
      authority.assertCurrent(state.context!, token);
      if (pages.get(state.context!.tabId) !== state) throw new Error('页面已更改。');
    });
    if (!response.taskId) throw new Error('桌面程序未返回已保存的任务编号。');
  });
}
async function createMenus(): Promise<void> {
  await api.contextMenus.removeAll();
  api.contextMenus.create({id: 'send-link', title: '使用 MaskPilot 下载链接', contexts: ['link']});
  api.contextMenus.create({id: 'send-media', title: '使用 MaskPilot 下载媒体', contexts: ['video', 'audio']});
  api.contextMenus.create({id: 'discover-page', title: '在 MaskPilot 中查看页面资源', contexts: ['page']});
}
api.runtime.onInstalled.addListener(() => { void createMenus().catch(() => {}); });
api.runtime.onStartup.addListener(() => { void createMenus().catch(() => {}); });
api.contextMenus.onClicked.addListener((info, tab) => {
  (async () => {
    if (info.menuItemId === 'discover-page') { await api.action.openPopup(); return; }
    const url = info.menuItemId === 'send-media' ? info.srcUrl : info.linkUrl;
    if (!url) throw new Error('没有可下载的地址。');
    const fields = await addFields(url, cookieContext(tab, isFirefox), undefined, tab?.url, info.menuItemId === 'send-media');
    await command('add', fields); await clearError();
  })().catch(error => report(error instanceof Error ? error.message : '下载请求失败。'));
});
api.downloads.onCreated.addListener(item => {
  (async () => {
    if (inFlight.has(item.id) || item.byExtensionId === api.runtime.id || item.state !== 'in_progress') return;
    if (!(await preferences()).autoCapture || await recovery.hasPending()) return;
    const url = safeHttpUrl(item.finalUrl || item.url);
    inFlight.add(item.id);
    try {
      const result = await handoffDownload({
        pauseBrowser: () => api.downloads.pause(item.id),
        prepareNative: async () => {
          const fields = await addFields(url, cookieContext(item, isFirefox), item.filename, item.referrer,
            /\.(m3u8|mpd)$/i.test(new URL(url).pathname));
          const response = await command('prepare', fields);
          if (!response.taskId) throw new Error('桌面程序没有返回已持久化的任务编号。');
          return response.taskId;
        },
        cancelBrowser: () => api.downloads.cancel(item.id), resumeBrowser: () => api.downloads.resume(item.id),
        cancelNative: async taskId => { await command('cancel', {taskId}); },
        resumeNative: async taskId => { await command('resume', {taskId}); }
      });
      if (result.error) await report(result.error); else await clearError();
    } finally { inFlight.delete(item.id); }
  })().catch(error => report(error instanceof Error ? error.message : '下载接管失败。'));
});
api.tabs.onRemoved.addListener(invalidate);
api.tabs.onUpdated.addListener((tabId, changes, tab) => {
  if (changes.status === 'loading' || changes.url) invalidate(tabId);
  if ((changes.status === 'complete' || changes.url) && tab.status === 'complete')
    void showPanel(tabId).catch(() => {});
});
api.storage.onChanged.addListener((changes, area) => {
  if (area !== 'local' || !changes.pagePanel) return;
  if (changes.pagePanel.newValue !== true) {
    for (const tabId of pages.keys()) void api.tabs.sendMessage(tabId, {kind: 'panel-hide'}, {frameId: 0}).catch(() => {});
    pages.clear(); authority.clear();
  } else void api.tabs.query({}).then(tabs => Promise.allSettled(tabs.slice(0, 64)
    .filter(tab => tab.id !== undefined).map(tab => showPanel(tab.id!))));
});
api.runtime.onMessage.addListener((message: unknown, sender, sendResponse) => {
  if (sender.id !== api.runtime.id || !isRecord(message)) return false;
  const trustedUi = sender.url === api.runtime.getURL('popup.html') && (sender.frameId === undefined || sender.frameId === 0);
  const content = !trustedUi && sender.tab?.id !== undefined && sender.frameId === 0;
  if (!trustedUi && !content) return false;
  (async () => {
    if (content) {
      if (!(await preferences()).pagePanel || sender.tab?.id === undefined || !sender.url)
        throw new Error('页面面板未启用。');
      const keys = message.kind === 'panel-refresh' ? ['kind', 'document'] : ['kind', 'document', 'token', 'ids'];
      if (!['panel-refresh', 'panel-submit'].includes(String(message.kind)) || Object.keys(message).some(key => !keys.includes(key)) ||
          typeof message.document !== 'string') throw new Error('页面消息无效。');
      const tabId = sender.tab.id;
      if (message.kind === 'panel-refresh') {
        const result = await scan(tabId);
        if (result.state.context!.documentId !== message.document || safeHttpUrl(sender.url) !== result.state.context!.url)
          throw new Error('页面已更改。');
        return {ok: true, model: result.model};
      }
      const state = pages.get(tabId);
      if (!state?.context || state.context.documentId !== message.document || safeHttpUrl(sender.url) !== state.context.url ||
          typeof message.token !== 'string') throw new Error('页面资源已失效，请刷新。');
      return {ok: true, ...(await submit(state, message.token, message.ids, true))};
    }
    if (typeof message.kind === 'string' && message.kind.startsWith('recovery-')) {
      const allowed: Record<string, string[]> = {
        'recovery-status': ['kind'], 'recovery-begin': ['kind', 'taskId'],
        'recovery-downloads': ['kind'], 'recovery-use': ['kind', 'downloadId'],
        'recovery-url': ['kind', 'url'], 'recovery-continue': ['kind'], 'recovery-cancel': ['kind']
      };
      const keys = allowed[message.kind];
      if (!keys || Object.keys(message).some(key => !keys.includes(key))) throw new Error('恢复界面消息无效。');
      switch (message.kind) {
        case 'recovery-status': return {ok: true, view: await recovery.status()};
        case 'recovery-begin':
          if (typeof message.taskId !== 'string') throw new Error('原任务编号无效。');
          return {ok: true, view: await recovery.begin(message.taskId)};
        case 'recovery-downloads': return {ok: true, downloads: await recovery.downloads()};
        case 'recovery-use':
          if (typeof message.downloadId !== 'number') throw new Error('下载记录编号无效。');
          return {ok: true, ...(await recovery.useDownload(message.downloadId))};
        case 'recovery-url': return {ok: true, ...(await recovery.useUrl(safeHttpUrl(message.url)))};
        case 'recovery-continue': return {ok: true, ...(await recovery.continue())};
        case 'recovery-cancel': return {ok: true, ...(await recovery.cancel())};
      }
    }
    if (message.kind === 'discover') {
      const [tab] = await api.tabs.query({active: true, currentWindow: true});
      if (tab?.id === undefined) throw new Error('请先打开一个网页。');
      const {state, model} = await scan(tab.id);
      return {ok: true, model, tabId: tab.id, pageUrl: state.context!.url};
    }
    if (message.kind === 'resource-batch') {
      if (typeof message.tabId !== 'number' || typeof message.token !== 'string') throw new Error('资源批次无效。');
      const state = pages.get(message.tabId);
      if (!state) throw new Error('页面资源已失效，请重新查找。');
      return {ok: true, ...(await submit(state, message.token, message.ids, false))};
    }
    if (message.kind === 'preferences') {
      if (typeof message.autoCapture !== 'boolean' || typeof message.includeCookies !== 'boolean' || typeof message.pagePanel !== 'boolean')
        throw new Error('设置无效。');
      // Revoking cookie/panel consent must remain possible when the host is unavailable.
      if (message.autoCapture && !(await preferences()).autoCapture) await command('ping');
      await api.storage.local.set({autoCapture: message.autoCapture, includeCookies: message.includeCookies, pagePanel: message.pagePanel});
      return {ok: true};
    }
    if (message.kind === 'native') {
      const request = validateRequest(message.request);
      if (request.command.startsWith('recovery.')) throw new Error('请通过恢复界面关联原任务。');
      if (request.command === 'add' || request.command === 'prepare') {
        const [tab] = await api.tabs.query({active: true, currentWindow: true});
        const fields = await addFields(request.url!, cookieContext(tab, isFirefox), request.filename, request.referrer, request.media);
        const response = await native({...request, ...fields}); await clearError(); return response;
      }
      return native(request);
    }
    throw new Error('扩展消息无效。');
  })().then(sendResponse, error => sendResponse({ok: false, error: error instanceof Error ? error.message : '操作失败。'}));
  return true;
});
