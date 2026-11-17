import { HOST_NAME, handoffDownload, isRecord, newRequestId, normalizeCandidates, safeHttpUrl,
  validateRequest, validateResponse, withTimeout, type Command, type NativeRequest, type NativeResponse } from './protocol.js';

const inFlight = new Set<number>();
interface Preferences { autoCapture: boolean; includeCookies: boolean; }
async function preferences(): Promise<Preferences> {
  const saved = await chrome.storage.local.get({autoCapture: false, includeCookies: false});
  return {autoCapture: saved.autoCapture === true, includeCookies: saved.includeCookies === true};
}
async function native(request: NativeRequest): Promise<NativeResponse> {
  const validated = validateRequest(request);
  const response = validateResponse(await withTimeout(chrome.runtime.sendNativeMessage(HOST_NAME, validated)), validated.requestId);
  if (!response.ok) throw new Error(response.error || '桌面程序未接受请求。');
  return response;
}
async function command(command: Command, fields: Partial<NativeRequest> = {}): Promise<NativeResponse> {
  return native({...fields, version: 1, requestId: newRequestId(), command});
}
async function cookiesFor(url: string): Promise<string | undefined> {
  if (!(await preferences()).includeCookies) return undefined;
  // Query only the selected URL, never the entire browser cookie store.
  const cookies = await chrome.cookies.getAll({url: safeHttpUrl(url)});
  const value = cookies.map(cookie => `${cookie.name}=${cookie.value}`).join('; ');
  if (value.length > 16_384) throw new Error('此地址的 Cookie 过长，请在桌面程序中处理登录信息。');
  return value || undefined;
}
async function addFields(url: string, filename?: string, referrer?: string, media = false): Promise<Partial<NativeRequest>> {
  const selected = safeHttpUrl(url);
  let safeReferrer: string | undefined;
  try { if (referrer) safeReferrer = safeHttpUrl(referrer); } catch { /* Page may be an internal browser page. */ }
  return {url: selected, filename, referrer: safeReferrer, cookie: await cookiesFor(selected), userAgent: navigator.userAgent, media};
}
async function report(error: string): Promise<void> {
  await chrome.storage.local.set({lastError: error.slice(0, 4096)});
  await chrome.action.setBadgeText({text: '!'});
  await chrome.action.setBadgeBackgroundColor({color: '#bd453b'});
}
async function clearError(): Promise<void> {
  await chrome.storage.local.remove('lastError');
  await chrome.action.setBadgeText({text: ''});
}
function createMenus(): void {
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({id: 'send-link', title: '使用 MaskPilot 下载链接', contexts: ['link']});
    chrome.contextMenus.create({id: 'send-media', title: '使用 MaskPilot 下载媒体', contexts: ['video', 'audio']});
    chrome.contextMenus.create({id: 'discover-page', title: '在 MaskPilot 中查看页面资源', contexts: ['page']});
  });
}
chrome.runtime.onInstalled.addListener(createMenus);
chrome.runtime.onStartup.addListener(createMenus);
chrome.contextMenus.onClicked.addListener((info, tab) => {
  (async () => {
    if (info.menuItemId === 'discover-page') {
      await chrome.action.openPopup();
      return;
    }
    const url = info.menuItemId === 'send-media' ? info.srcUrl : info.linkUrl;
    if (!url) throw new Error('没有可下载的地址。');
    await command('add', await addFields(url, undefined, tab?.url, info.menuItemId === 'send-media'));
    await clearError();
  })().catch(error => report(error instanceof Error ? error.message : '下载请求失败。'));
});
chrome.downloads.onCreated.addListener(item => {
  (async () => {
    if (inFlight.has(item.id) || item.byExtensionId === chrome.runtime.id || item.state !== 'in_progress') return;
    const settings = await preferences();
    if (!settings.autoCapture) return;
    const url = safeHttpUrl(item.finalUrl || item.url);
    inFlight.add(item.id);
    try {
      const result = await handoffDownload({
        pauseBrowser: () => chrome.downloads.pause(item.id),
        prepareNative: async () => {
          const response = await command('prepare', await addFields(url, item.filename, item.referrer, /\.(m3u8|mpd)$/i.test(new URL(url).pathname)));
          if (!response.taskId) throw new Error('桌面程序没有返回已持久化的任务编号。');
          return response.taskId;
        },
        cancelBrowser: () => chrome.downloads.cancel(item.id),
        resumeBrowser: () => chrome.downloads.resume(item.id),
        cancelNative: async taskId => { await command('cancel', {taskId}); },
        resumeNative: async taskId => { await command('resume', {taskId}); }
      });
      if (result.error) await report(result.error);
      else await clearError();
    } finally { inFlight.delete(item.id); }
  })().catch(error => report(error instanceof Error ? error.message : '下载接管失败。'));
});
chrome.runtime.onMessage.addListener((message: unknown, sender, sendResponse) => {
  const trustedUi = sender.url === chrome.runtime.getURL('popup.html');
  if (sender.id !== chrome.runtime.id || (sender.tab && !trustedUi) || !isRecord(message)) return false;
  (async () => {
    if (message.kind === 'discover') {
      const [tab] = await chrome.tabs.query({active: true, currentWindow: true});
      if (!tab?.id || !tab.url) throw new Error('请先打开一个网页。');
      safeHttpUrl(tab.url);
      const results = await chrome.scripting.executeScript({target: {tabId: tab.id}, files: ['content.js']});
      return {ok: true, items: normalizeCandidates(results[0]?.result), pageUrl: tab.url};
    }
    if (message.kind === 'preferences') {
      if (typeof message.autoCapture !== 'boolean' || typeof message.includeCookies !== 'boolean') throw new Error('设置无效。');
      if (message.autoCapture) await command('ping');
      await chrome.storage.local.set({autoCapture: message.autoCapture, includeCookies: message.includeCookies});
      return {ok: true};
    }
    if (message.kind === 'native') {
      const request = validateRequest(message.request);
      if (request.command === 'add' || request.command === 'prepare') {
        const supplied = await addFields(request.url!, request.filename, request.referrer, request.media);
        const response = await native({...request, ...supplied});
        await clearError();
        return response;
      }
      return native(request);
    }
    throw new Error('扩展消息无效。');
  })().then(result => sendResponse(result), error => sendResponse({ok: false, error: error instanceof Error ? error.message : '操作失败。'}));
  return true;
});
