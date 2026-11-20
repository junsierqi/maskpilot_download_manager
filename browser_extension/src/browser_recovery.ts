import {cookieContext, cookieHeader, type CookieContext} from './browser_context.js';
import {isRecord, safeHttpUrl, type Command, type NativeRequest, type NativeResponse} from './protocol.js';
import type {MethodDownload} from './download_methods.js';

const STORAGE_KEY = 'browserRecovery';
const identifier = /^[a-f0-9]{32}$/;
interface Flow {
  version: 1;
  token: string;
  taskId: string;
  name: string;
  tabId: number;
  storeId?: string;
  startedAt: number;
  expiresAt: number;
  phase: 'selecting' | 'applied';
  downloadId?: number;
  wasPaused?: boolean;
  selectedUrl?: string;
}
export interface RecoveryView {
  active: boolean;
  name?: string;
  taskId?: string;
  expiresAt?: number;
  phase?: Flow['phase'];
  expired?: boolean;
}
export interface RecoveryDownload {id: number; name: string; url: string; paused: boolean;}
interface Dependencies {
  api: typeof chrome;
  firefox: boolean;
  send(command: Command, fields?: Partial<NativeRequest>): Promise<NativeResponse>;
  includeCookies(): Promise<boolean>;
  userAgent(): string;
  isGetDownload(item: MethodDownload): Promise<boolean>;
  now?(): number;
}
type Tab = chrome.tabs.Tab & {cookieStoreId?: string};
type Download = chrome.downloads.DownloadItem & {cookieStoreId?: string};
const delay = (milliseconds: number) => new Promise(resolve => setTimeout(resolve, milliseconds));

// Only the extension's trusted popup reaches this controller. Website/panel messages never do.
export class BrowserRecoveryController {
  private busy = false;
  private storageReady?: Promise<void>;
  constructor(private readonly deps: Dependencies) {}
  private now(): number { return this.deps.now?.() ?? Date.now(); }
  private async storage(): Promise<chrome.storage.StorageArea> {
    const area = this.deps.api.storage.session;
    if (!area) throw new Error('浏览器不支持安全的内存会话，请使用桌面更新请求。');
    this.storageReady ??= (async () => {
      if (typeof area.setAccessLevel === 'function')
        await area.setAccessLevel({accessLevel: 'TRUSTED_CONTEXTS' as chrome.storage.AccessLevel});
    })();
    await this.storageReady;
    return area;
  }
  private async read(): Promise<Flow | undefined> {
    const raw: unknown = (await (await this.storage()).get(STORAGE_KEY))[STORAGE_KEY];
    if (raw === undefined) return undefined;
    if (!isRecord(raw) || raw.version !== 1 || typeof raw.token !== 'string' || !identifier.test(raw.token) ||
        Object.keys(raw).some(key => !['version', 'token', 'taskId', 'name', 'tabId', 'storeId', 'startedAt',
          'expiresAt', 'phase', 'downloadId', 'wasPaused', 'selectedUrl'].includes(key)) ||
        typeof raw.taskId !== 'string' || !identifier.test(raw.taskId) || typeof raw.name !== 'string' || raw.name.length > 720 ||
        !Number.isSafeInteger(raw.tabId) || (raw.tabId as number) < -1 || !Number.isSafeInteger(raw.startedAt) ||
        !Number.isSafeInteger(raw.expiresAt) || (raw.phase !== 'selecting' && raw.phase !== 'applied') ||
        (raw.storeId !== undefined && (typeof raw.storeId !== 'string' || !/^[\w-]{1,128}$/.test(raw.storeId))) ||
        (raw.downloadId !== undefined && (!Number.isSafeInteger(raw.downloadId) || (raw.downloadId as number) < 0)) ||
        (raw.wasPaused !== undefined && typeof raw.wasPaused !== 'boolean') ||
        (raw.selectedUrl !== undefined && (typeof raw.selectedUrl !== 'string' || safeHttpUrl(raw.selectedUrl) !== raw.selectedUrl)) ||
        (raw.downloadId !== undefined && (typeof raw.wasPaused !== 'boolean' || typeof raw.selectedUrl !== 'string')) ||
        (raw.downloadId === undefined && (raw.wasPaused !== undefined || raw.selectedUrl !== undefined)))
      throw new Error('恢复会话无效，请取消会话并在浏览器下载列表检查暂停项。');
    return raw as unknown as Flow;
  }
  private async save(flow: Flow): Promise<void> { await (await this.storage()).set({[STORAGE_KEY]: flow}); }
  private async clear(): Promise<void> { await (await this.storage()).remove(STORAGE_KEY); }
  private async exclusive<T>(action: () => Promise<T>): Promise<T> {
    if (this.busy) throw new Error('恢复操作正在进行，请稍候。');
    this.busy = true;
    try { return await action(); } finally { this.busy = false; }
  }
  private expired(flow: Flow): boolean {
    return this.now() < flow.startedAt || this.now() >= flow.expiresAt * 1000;
  }
  private async requireFlow(phase?: Flow['phase']): Promise<Flow> {
    const flow = await this.read();
    if (!flow || this.expired(flow) || (phase && flow.phase !== phase))
      throw new Error('恢复会话已失效或阶段已改变，请取消后重新开始。');
    return flow;
  }
  private context(source: Tab | Download): CookieContext {
    const context = cookieContext(source, this.deps.firefox);
    if (context.incognito || (context.firefox && (!context.storeId || !/^[\w-]{1,128}$/.test(context.storeId) || /private/i.test(context.storeId))))
      throw new Error('无法安全绑定此浏览器容器，请保留浏览器下载。');
    return context;
  }
  private matches(flow: Flow, context: CookieContext): boolean {
    return !context.incognito && context.storeId === flow.storeId;
  }
  private async tab(flow: Flow): Promise<Tab> {
    if (flow.tabId < 0) throw new Error('来源页尚未打开，请取消后重新开始。');
    const tab: Tab = await this.deps.api.tabs.get(flow.tabId);
    if (!this.matches(flow, this.context(tab))) throw new Error('来源页的 Cookie 容器已改变，请重新开始。');
    return tab;
  }
  private async item(flow: Flow, id: number, requireGet = true): Promise<Download> {
    if (!Number.isSafeInteger(id) || id < 0) throw new Error('浏览器下载编号无效。');
    const items: Download[] = await this.deps.api.downloads.search({id});
    const item = items.find(value => value.id === id);
    if (!item || !Number.isFinite(Date.parse(item.startTime)) || Date.parse(item.startTime) < flow.startedAt - 1000 ||
        !this.matches(flow, this.context(item)))
      throw new Error('所选下载不属于此次恢复或当前 Cookie 容器。');
    safeHttpUrl(item.finalUrl || item.url);
    if (requireGet && !(await this.deps.isGetDownload(item)))
      throw new Error('仅可选择明确观察到 GET 的下载；POST 或方法未知的记录请保留在浏览器。');
    return item;
  }
  private async restoreBrowser(flow: Flow): Promise<void> {
    if (flow.downloadId === undefined || flow.wasPaused !== false) return;
    const item = await this.item(flow, flow.downloadId, false);
    if (item.paused && item.state !== 'complete') await this.deps.api.downloads.resume(item.id);
  }
  private async material(flow: Flow, url: string, source: CookieContext, referrer?: string): Promise<Partial<NativeRequest>> {
    const selected = safeHttpUrl(url);
    const cookie = await cookieHeader(selected, source, await this.deps.includeCookies(), query => this.deps.api.cookies.getAll(query));
    await this.tab(flow);
    const current = await this.requireFlow(flow.phase);
    if (current.token !== flow.token) throw new Error('恢复会话已改变。');
    let safeReferrer: string | undefined;
    try { if (referrer) safeReferrer = safeHttpUrl(referrer); } catch { /* Non-HTTP browser page. */ }
    return {recoveryToken: flow.token, url: selected, cookie, referrer: safeReferrer, userAgent: this.deps.userAgent()};
  }
  async status(): Promise<RecoveryView> {
    const flow = await this.read();
    return flow ? {active: true, name: flow.name, taskId: flow.taskId, expiresAt: flow.expiresAt,
      phase: flow.phase, expired: this.expired(flow)} : {active: false};
  }
  async hasPending(): Promise<boolean> { return this.busy || !!(await this.read()); }
  async begin(taskId: string): Promise<RecoveryView> {
    return this.exclusive(async () => {
      if (await this.read()) throw new Error('请先完成或取消当前恢复会话。');
      if (!identifier.test(taskId)) throw new Error('原任务编号无效。');
      const [active] = await this.deps.api.tabs.query({active: true, currentWindow: true}) as Tab[];
      if (active?.id === undefined) throw new Error('请先打开来源网页。');
      const context = this.context(active);
      await this.deps.send('pause', {taskId});
      const deadline = performance.now() + 10_000;
      for (;;) {
        const result = await this.deps.send('list');
        const task = result.tasks?.find(value => value.id === taskId);
        if (!task?.canRecover) throw new Error('此任务不支持浏览器辅助恢复。');
        if (task.state === 'Paused') break;
        if (performance.now() >= deadline) throw new Error('请等待原任务暂停完成，再重新开始恢复。');
        await delay(100);
      }
      const session = await this.deps.send('recovery.begin', {taskId});
      if (typeof session.recoveryToken !== 'string' || !identifier.test(session.recoveryToken) || session.taskId !== taskId ||
          typeof session.name !== 'string' || session.name.length > 720 || !Number.isSafeInteger(session.expiresAt) ||
          session.expiresAt! * 1000 <= this.now() || session.expiresAt! * 1000 > this.now() + 601_000)
        throw new Error('桌面返回的恢复会话无效。');
      const pageUrl = safeHttpUrl(session.pageUrl);
      const flow: Flow = {version: 1, token: session.recoveryToken, taskId, name: session.name,
        tabId: -1, ...(context.storeId === undefined ? {} : {storeId: context.storeId}),
        startedAt: this.now(), expiresAt: session.expiresAt!, phase: 'selecting'};
      try {
        await this.save(flow); // Suppress automatic capture before opening a possible download URL.
        let existing = '';
        try { existing = safeHttpUrl(active.url); } catch { /* Extension/system tab. */ }
        const tab: Tab = existing === pageUrl ? active : await this.deps.api.tabs.create({url: pageUrl, active: true,
          windowId: active.windowId, ...(this.deps.firefox ? {cookieStoreId: context.storeId} : {})});
        if (tab.id === undefined || !this.matches(flow, this.context(tab))) throw new Error('无法在原 Cookie 容器打开来源页。');
        flow.tabId = tab.id;
        await this.save(flow);
        return this.status();
      } catch (error) {
        await this.clear();
        await this.deps.send('recovery.cancel', {recoveryToken: flow.token}).catch(() => {});
        throw error;
      }
    });
  }
  async downloads(): Promise<RecoveryDownload[]> {
    const flow = await this.requireFlow('selecting');
    await this.tab(flow);
    const items: Download[] = await this.deps.api.downloads.search({startedAfter: new Date(flow.startedAt - 1000).toISOString(),
      orderBy: ['-startTime'], limit: 20});
    // Observation waits are independent; a full page of unknown records must not take 20 wait windows.
    const checked = await Promise.all(items.slice(0, 20).map(async item => {
      try {
        if ((item.state !== 'in_progress' && !item.paused) || item.byExtensionId === this.deps.api.runtime.id ||
            !this.matches(flow, this.context(item))) return;
        if (!(await this.deps.isGetDownload(item))) return;
        return {id: item.id, name: item.filename.split(/[\\/]/).at(-1) || '浏览器下载',
          url: safeHttpUrl(item.finalUrl || item.url), paused: item.paused};
      } catch { return; /* Unsupported record remains in the browser. */ }
    }));
    return checked.filter((item): item is RecoveryDownload => item !== undefined);
  }
  private async finish(flow: Flow): Promise<{message: string}> {
    if (flow.downloadId !== undefined) {
      const item = await this.item(flow, flow.downloadId, false);
      if (item.state === 'complete') throw new Error('浏览器副本已完成；请在桌面核对文件后处理原任务。');
      if (safeHttpUrl(item.finalUrl || item.url) !== flow.selectedUrl) throw new Error('浏览器下载地址已改变，请取消后重新开始。');
      // Already cancelled records have no partial data; never restart those browser copies.
      if (item.state === 'in_progress' || item.paused) {
        try { await this.deps.api.downloads.cancel(item.id); }
        catch {
          await this.restoreBrowser(flow).catch(() => {});
          return {message: '浏览器副本未能取消，原任务已更新并保持暂停。请在桌面与浏览器中选择一份继续。'};
        }
      }
    }
    try {
      const result = await this.deps.send('recovery.resume', {recoveryToken: flow.token});
      if (result.taskId !== flow.taskId) throw new Error('恢复任务编号不匹配。');
      await this.clear();
      return {message: '已更新并恢复原任务，未创建新的桌面任务。'};
    } catch {
      return {message: '原任务已保存，启动状态需要在桌面确认；若仍显示恢复会话，可再次检查或取消。'};
    }
  }
  async useDownload(id: number): Promise<{message: string}> {
    return this.exclusive(async () => {
      const flow = await this.requireFlow('selecting');
      await this.tab(flow);
      const item = await this.item(flow, id);
      if (item.state !== 'in_progress' && !item.paused) throw new Error('请选择正在下载或暂停的浏览器记录。');
      flow.downloadId = id; flow.wasPaused = item.paused;
      flow.selectedUrl = safeHttpUrl(item.finalUrl || item.url);
      await this.save(flow);
      let applied = false;
      try {
        if (!item.paused) await this.deps.api.downloads.pause(id);
        const current = await this.item(flow, id);
        if (!current.paused || safeHttpUrl(current.finalUrl || current.url) !== flow.selectedUrl)
          throw new Error('浏览器下载未保持暂停或地址已改变。');
        const fields = await this.material(flow, flow.selectedUrl, this.context(current), current.referrer);
        const response = await this.deps.send('recovery.apply', fields);
        if (response.taskId !== flow.taskId) throw new Error('原任务确认编号不匹配。');
        applied = true; flow.phase = 'applied';
        await this.save(flow);
      } catch {
        let restored = true;
        try { await this.restoreBrowser(flow); } catch { restored = false; }
        throw new Error((applied ? '原任务已更新并保持暂停。' : '更新未得到确认，原任务不会自动开始。') +
          (restored ? '浏览器下载已保留；需要登录时请启用 Cookie 转发后重试。' : '请到浏览器下载列表恢复该下载。'));
      }
      return this.finish(flow);
    });
  }
  async continue(): Promise<{message: string}> {
    return this.exclusive(async () => this.finish(await this.requireFlow('applied')));
  }
  async useUrl(url: string): Promise<{message: string}> {
    return this.exclusive(async () => {
      const flow = await this.requireFlow('selecting'), tab = await this.tab(flow);
      const fields = await this.material(flow, safeHttpUrl(url), this.context(tab), tab.url);
      const result = await this.deps.send('recovery.apply', fields);
      if (result.taskId !== flow.taskId) throw new Error('原任务确认编号不匹配。');
      await this.clear();
      await this.deps.send('recovery.cancel', {recoveryToken: flow.token}).catch(() => {});
      return {message: '原任务已更新并保持暂停，请在桌面核对浏览器副本后开始。'};
    });
  }
  async cancel(): Promise<{message: string}> {
    return this.exclusive(async () => {
      let flow: Flow | undefined;
      try { flow = await this.read(); } catch { await this.clear(); return {message: '已清除无效恢复会话，请检查浏览器暂停项。'}; }
      if (!flow) return {message: '没有待处理的恢复会话。'};
      let restored = true;
      try { await this.restoreBrowser(flow); } catch { restored = false; }
      await this.clear();
      await this.deps.send('recovery.cancel', {recoveryToken: flow.token}).catch(() => {});
      return {message: restored ? '恢复会话已取消，原任务和浏览器文件均未删除。' :
        '恢复会话已取消，原任务未删除；请在浏览器下载列表手动恢复暂停项。'};
    });
  }
}
