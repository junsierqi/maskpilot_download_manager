import { newRequestId, isRecord, safeHttpUrl, validateRequest, withTimeout,
  type Command, type NativeRequest, type NativeTask } from './protocol.js';
import {browserApi as api} from './browser_api.js';
import type {PanelItem} from './panel_authority.js';

function element<T extends HTMLElement>(id: string): T {
  const result = document.getElementById(id);
  if (!result) throw new Error('Missing UI element: ' + id);
  return result as T;
}
const urlInput = element<HTMLInputElement>('url');
const feedback = element<HTMLDivElement>('feedback');
let candidates: PanelItem[] = [];
let resourceToken = '', resourceTab = -1;
let pageUrl = '';
function show(message: string, error = false): void {
  feedback.textContent = message;
  feedback.classList.toggle('error', error);
  feedback.hidden = false;
}
async function call(message: object): Promise<Record<string, unknown>> {
  const result = await withTimeout(api.runtime.sendMessage(message));
  if (!result || !result.ok) throw new Error(typeof result?.error === 'string' ? result.error : '操作失败。');
  return result;
}
async function native(command: Command, fields: Partial<NativeRequest> = {}): Promise<Record<string, unknown>> {
  return call({kind: 'native', request: validateRequest({...fields, version: 1, command, requestId: newRequestId()})});
}
async function busy(button: HTMLButtonElement, action: () => Promise<void>): Promise<void> {
  button.disabled = true;
  try { await action(); } catch (error) { show(error instanceof Error ? error.message : '操作失败。', true); }
  finally { button.disabled = false; }
}
async function checkConnection(): Promise<void> {
  const button = element<HTMLButtonElement>('connection');
  button.textContent = '检测中';
  button.className = 'status';
  try {
    await native('ping');
    button.textContent = '● 桌面已连接';
    button.className = 'status connected';
  } catch {
    button.textContent = '桌面未连接';
    button.className = 'status offline';
  }
}
element<HTMLButtonElement>('connection').addEventListener('click', () => {
  void busy(element<HTMLButtonElement>('connection'), checkConnection);
});
const send = element<HTMLButtonElement>('send');
send.addEventListener('click', () => { void busy(send, async () => {
  const url = safeHttpUrl(urlInput.value.trim());
  await native('add', {url, referrer: pageUrl || undefined, media: element<HTMLInputElement>('media-url').checked});
  show('已发送到 MaskPilot。');
  urlInput.value = '';
}); });
urlInput.addEventListener('keydown', event => { if (event.key === 'Enter' && !send.disabled) send.click(); });
function setPanel(resources: boolean): void {
  element('resources-panel').hidden = !resources;
  element('tasks-panel').hidden = resources;
  for (const [id, selected] of [['resources-tab', resources], ['tasks-tab', !resources]] as const) {
    element(id).classList.toggle('selected', selected);
    element(id).setAttribute('aria-selected', String(selected));
  }
}
element('resources-tab').addEventListener('click', () => setPanel(true));
element('tasks-tab').addEventListener('click', () => { setPanel(false); void loadTasks(); });
function renderCandidates(): void {
  const root = element('resources'); root.replaceChildren();
  element('selection-row').hidden = candidates.length === 0;
  if (!candidates.length) {
    const empty = document.createElement('div'); empty.className = 'empty';
    empty.textContent = '未发现可直接下载的文件或媒体。可手动粘贴链接。'; root.append(empty); return;
  }
  candidates.forEach((candidate, index) => {
    const row = document.createElement('label'); row.className = 'resource';
    const checkbox = document.createElement('input'); checkbox.type = 'checkbox'; checkbox.value = String(index);
    checkbox.disabled = candidate.accepted;
    const copy = document.createElement('div'); copy.className = 'item-copy';
    const title = document.createElement('div'); title.className = 'item-title';
    title.textContent = candidate.name || new URL(candidate.url).pathname.split('/').at(-1) || '下载文件';
    const address = document.createElement('div'); address.className = 'item-url'; address.textContent = candidate.url;
    const kind = document.createElement('span'); kind.className = 'kind'; kind.textContent = candidate.accepted ? '已发送' : (candidate.kind === 'media' ? '媒体' : '文件');
    copy.append(title, address); row.append(checkbox, copy, kind); root.append(row);
  });
  element<HTMLInputElement>('select-all').checked = false;
}
const discover = element<HTMLButtonElement>('discover');
discover.addEventListener('click', () => { void busy(discover, async () => {
  const response = await call({kind: 'discover'});
  if (!isRecord(response.model) || typeof response.model.token !== 'string' || !Array.isArray(response.model.items) || typeof response.tabId !== 'number')
    throw new Error('页面资源响应无效。');
  candidates = response.model.items as PanelItem[];
  resourceToken = response.model.token; resourceTab = response.tabId;
  pageUrl = typeof response.pageUrl === 'string' ? safeHttpUrl(response.pageUrl) : '';
  element('resources-caption').textContent = `发现 ${candidates.length} 项资源`;
  renderCandidates();
}); });
element<HTMLInputElement>('select-all').addEventListener('change', event => {
  for (const checkbox of document.querySelectorAll<HTMLInputElement>('#resources input[type=checkbox]:not(:disabled)'))
    checkbox.checked = (event.target as HTMLInputElement).checked;
});
const selected = element<HTMLButtonElement>('send-selected');
selected.addEventListener('click', () => { void busy(selected, async () => {
  const chosen = [...document.querySelectorAll<HTMLInputElement>('#resources input:checked')]
    .map(checkbox => candidates[Number(checkbox.value)]).filter((candidate): candidate is PanelItem => !!candidate && !candidate.accepted);
  if (!chosen.length) throw new Error('请先选择至少一项资源。');
  const response = await call({kind: 'resource-batch', tabId: resourceTab, token: resourceToken, ids: chosen.map(item => item.id)});
  const accepted = new Set(Array.isArray(response.accepted) ? response.accepted : []);
  const remaining = new Set(Array.isArray(response.remaining) ? response.remaining : []);
  for (const item of candidates) if (accepted.has(item.id)) item.accepted = true;
  renderCandidates();
  for (const checkbox of document.querySelectorAll<HTMLInputElement>('#resources input[type=checkbox]'))
    checkbox.checked = remaining.has(candidates[Number(checkbox.value)]?.id);
  show(`已发送 ${accepted.size} 项资源。${typeof response.error === 'string' ? response.error : ''}`, !!response.error);
}); });
async function loadTasks(): Promise<void> {
  const root = element('tasks'); root.textContent = '正在读取任务…';
  try {
    const response = await native('list');
    const tasks = Array.isArray(response.tasks) ? response.tasks.slice(0, 100) as NativeTask[] : [];
    root.replaceChildren();
    if (!tasks.length) { const empty = document.createElement('div'); empty.className = 'empty'; empty.textContent = '桌面中暂无任务。'; root.append(empty); }
    for (const task of tasks) {
      if (!task || typeof task.id !== 'string' || !/^[a-fA-F0-9]{32}$/.test(task.id)) continue;
      const row = document.createElement('div'); row.className = 'task';
      const top = document.createElement('div'); top.className = 'task-top';
      const name = document.createElement('span'); name.className = 'item-title'; name.textContent = task.name || task.id.slice(0, 8);
      const state = document.createElement('span'); state.className = 'task-state'; state.textContent = task.state || '';
      top.append(name, state); row.append(top);
      const controls = document.createElement('div'); controls.className = 'task-controls';
      for (const [action, text] of [['resume', '开始'], ['pause', '暂停'], ['cancel', '取消']] as const) {
        const button = document.createElement('button'); button.textContent = text;
        button.addEventListener('click', () => { void busy(button, async () => { await native(action, {taskId: task.id}); await loadTasks(); }); });
        controls.append(button);
      }
      row.append(controls); root.append(row);
    }
  } catch (error) { root.textContent = '连接桌面程序后可查看任务。'; show(error instanceof Error ? error.message : '读取失败。', true); }
}
element('refresh-tasks').addEventListener('click', () => { void loadTasks(); });
const autoCapture = element<HTMLInputElement>('auto-capture');
const includeCookies = element<HTMLInputElement>('include-cookies');
const pagePanel = element<HTMLInputElement>('page-panel');
async function savePreferences(): Promise<void> {
  autoCapture.disabled = includeCookies.disabled = pagePanel.disabled = true;
  try { await call({kind: 'preferences', autoCapture: autoCapture.checked, includeCookies: includeCookies.checked, pagePanel: pagePanel.checked}); }
  catch (error) {
    const saved = await api.storage.local.get({autoCapture: false, includeCookies: false, pagePanel: false});
    autoCapture.checked = saved.autoCapture === true; includeCookies.checked = saved.includeCookies === true;
    pagePanel.checked = saved.pagePanel === true;
    show(error instanceof Error ? error.message : '设置未保存。', true);
  } finally { autoCapture.disabled = includeCookies.disabled = pagePanel.disabled = false; }
}
autoCapture.addEventListener('change', () => { void savePreferences(); });
includeCookies.addEventListener('change', () => { void savePreferences(); });
pagePanel.addEventListener('change', () => { void savePreferences(); });
void (async () => {
  const saved = await api.storage.local.get({autoCapture: false, includeCookies: false, pagePanel: false, lastError: ''});
  autoCapture.checked = saved.autoCapture === true; includeCookies.checked = saved.includeCookies === true;
  pagePanel.checked = saved.pagePanel === true;
  if (typeof saved.lastError === 'string' && saved.lastError) show(saved.lastError, true);
  try { const [tab] = await api.tabs.query({active: true, currentWindow: true}); if (tab?.url) pageUrl = safeHttpUrl(tab.url); } catch { /* Browser pages are not downloadable. */ }
  await checkConnection();
})();
