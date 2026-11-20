import {safeHttpUrl} from './protocol.js';

export interface MethodDownload {
  url: string; finalUrl?: string; startTime: string; incognito?: boolean;
  cookieStoreId?: string; byExtensionId?: string;
}
interface Observation {
  requestId: string; url: string; method: string; timeStamp: number;
  incognito?: boolean; cookieStoreId?: string;
}
interface Namespace {incognito: boolean; cookieStoreId?: string;}
interface Entry {
  url: string; method: string; first: number; response?: number; touched: number;
  context?: Namespace;
}
const retention = 10 * 60 * 1000;

// Browsers expose no shared request ID on DownloadItem. Only an unambiguous observed response is usable.
// This bounded, process-local metadata is deliberately lost on service-worker restart.
export class DownloadMethodLedger {
  private readonly entries = new Map<string, Entry>();
  constructor(private readonly firefox: boolean, private readonly now = () => Date.now()) {}
  private prune(): void {
    const now = this.now();
    for (const [key, entry] of this.entries)
      if (now < entry.touched || now - entry.touched > retention) this.entries.delete(key);
  }
  get size(): number { this.prune(); return this.entries.size; }
  observe(details: Observation, response = false): string | undefined {
    this.prune();
    if (typeof details.requestId !== 'string' || details.requestId.length > 128 || !details.requestId ||
        typeof details.method !== 'string' || details.method.length > 32 || !details.method ||
        !Number.isFinite(details.timeStamp) || details.timeStamp > this.now() + 5000) return;
    let url: string;
    try { url = safeHttpUrl(details.url); } catch { return; }
    const key = `${details.requestId}\n${details.method}\n${url}`;
    const entry = this.entries.get(key) ?? {url, method: details.method, first: details.timeStamp,
      touched: this.now()};
    if (response) entry.response = details.timeStamp;
    entry.touched = this.now();
    if (typeof details.incognito === 'boolean' &&
        (!this.firefox || (typeof details.cookieStoreId === 'string' && /^[\w-]{1,128}$/.test(details.cookieStoreId))))
      entry.context = {incognito: details.incognito, cookieStoreId: details.cookieStoreId};
    this.entries.delete(key);
    this.entries.set(key, entry);
    while (this.entries.size > 256) this.entries.delete(this.entries.keys().next().value!);
    return key;
  }
  bindContext(key: string, context: Namespace): void {
    const entry = this.entries.get(key);
    if (entry) entry.context = {incognito: context.incognito, cookieStoreId: context.cookieStoreId};
  }
  isGet(item: MethodDownload): boolean {
    this.prune();
    if (item.byExtensionId || item.incognito !== false ||
        (this.firefox && (!item.cookieStoreId || !/^[\w-]{1,128}$/.test(item.cookieStoreId) ||
          /private/i.test(item.cookieStoreId)))) return false;
    let url: string;
    try { url = safeHttpUrl(item.finalUrl || item.url); } catch { return false; }
    const start = Date.parse(item.startTime);
    if (!Number.isFinite(start) || start > this.now() + 1000) return false;
    const matches = [...this.entries.values()].filter(entry => {
      if (entry.url !== url || start < entry.first - 1000 ||
          start > (entry.response ?? this.now()) + 2000) return false;
      // Unknown context might be the same namespace; it cannot safely be ignored.
      return !entry.context || (entry.context.incognito === item.incognito &&
        (!this.firefox || entry.context.cookieStoreId === item.cookieStoreId));
    });
    const entry = matches[0];
    return matches.length === 1 && entry?.method === 'GET' && entry.response !== undefined &&
      entry.context?.incognito === false && (!this.firefox || entry.context.cookieStoreId === item.cookieStoreId);
  }
}

export function observeDownloadMethods(api: typeof chrome, firefox: boolean): (item: MethodDownload) => Promise<boolean> {
  if (!api.webRequest?.onBeforeRequest || !api.webRequest?.onHeadersReceived)
    return async () => false;
  const ledger = new DownloadMethodLedger(firefox);
  let lookups = 0;
  const observe = (details: Observation & {tabId: number}, response: boolean): void => {
    const metadata = details;
    const key = ledger.observe(metadata, response);
    // Firefox supplies a trusted namespace directly. Chrome requires resolving the originating tab.
    const completeContext = typeof metadata.incognito === 'boolean' &&
      (!firefox || (typeof metadata.cookieStoreId === 'string' && /^[\w-]{1,128}$/.test(metadata.cookieStoreId)));
    if (!key || completeContext || details.tabId < 0 || lookups >= 64) return;
    ++lookups;
    void api.tabs.get(details.tabId).then(tab => {
      ledger.bindContext(key, {incognito: tab.incognito,
        cookieStoreId: (tab as chrome.tabs.Tab & {cookieStoreId?: string}).cookieStoreId});
    }, () => {}).finally(() => --lookups);
  };
  const filter = {urls: ['http://*/*', 'https://*/*']};
  // No requestBody, responseHeaders, extraHeaders or blocking collection options.
  api.webRequest.onBeforeRequest.addListener(details => { observe(details, false); return undefined; }, filter);
  api.webRequest.onHeadersReceived.addListener(details => { observe(details, true); return undefined; }, filter);
  return async item => {
    for (let attempt = 0; attempt < 20; ++attempt) {
      if (ledger.isGet(item)) return true;
      await new Promise(resolve => setTimeout(resolve, 25));
    }
    return false;
  };
}
