import {newRequestId, normalizeCandidates, safeHttpUrl, type Candidate} from './protocol.js';

export interface PageContext {tabId: number; url: string; documentId?: string;}
export interface PanelItem extends Candidate {id: string; accepted: boolean;}
export interface PanelModel {token: string; items: PanelItem[];}
interface Entry {item: PanelItem; requestId: string;}
interface Session {context: PageContext; token: string; entries: Entry[]; used: number; busy: boolean;}
export interface BatchResult {accepted: string[]; remaining: string[]; error?: string;}
export class PanelAuthority {
  private readonly sessions = new Map<number, Session>();
  constructor(private readonly now = () => performance.now(), private readonly id = newRequestId,
    private readonly ttl = 10 * 60_000) {}
  invalidate(tabId: number): void { this.sessions.delete(tabId); }
  clear(): void { this.sessions.clear(); }
  private same(a: PageContext, b: PageContext): boolean {
    return a.tabId === b.tabId && a.url === b.url && a.documentId === b.documentId;
  }
  private prune(): void {
    for (const [id, session] of this.sessions)
      if (this.now() - session.used >= this.ttl) this.sessions.delete(id);
  }
  open(context: PageContext, candidates: unknown): PanelModel {
    if (!Number.isInteger(context.tabId) || context.tabId < 0) throw new Error('页面编号无效。');
    context = {...context, url: safeHttpUrl(context.url)};
    this.prune();
    const previous = this.sessions.get(context.tabId);
    if (previous?.busy) throw new Error('请等待当前批次完成。');
    const retained = previous && this.same(previous.context, context) ? previous.entries : [];
    const entries = normalizeCandidates(candidates).map(candidate => {
      const old = retained.find(entry => entry.item.url === candidate.url);
      return old ?? {item: {...candidate, id: this.id(), accepted: false}, requestId: this.id()};
    });
    if (!this.sessions.has(context.tabId) && this.sessions.size >= 64)
      this.sessions.delete(this.sessions.keys().next().value!);
    const session: Session = {context, token: this.id(), entries, used: this.now(), busy: false};
    this.sessions.set(context.tabId, session);
    return {token: session.token, items: entries.map(entry => ({...entry.item}))};
  }
  private get(context: PageContext, token: string): Session {
    this.prune();
    const session = this.sessions.get(context.tabId);
    if (!session || !this.same(session.context, context) || session.token !== token)
      throw new Error('页面资源已失效，请重新查找。');
    session.used = this.now();
    return session;
  }
  assertCurrent(context: PageContext, token: string): void { this.get(context, token); }
  async submit(context: PageContext, token: string, ids: unknown,
    send: (candidate: Candidate, requestId: string) => Promise<void>): Promise<BatchResult> {
    const session = this.get(context, token);
    if (!Array.isArray(ids) || !ids.length || ids.length > 200 ||
        ids.some(id => typeof id !== 'string' || !/^[a-f0-9]{32}$/.test(id)) || new Set(ids).size !== ids.length)
      throw new Error('所选资源无效。');
    const entries = ids.map(id => session.entries.find(entry => entry.item.id === id));
    if (entries.some(entry => !entry)) throw new Error('资源不属于当前页面。');
    if (session.busy) throw new Error('当前批次正在发送，请稍候。');
    session.busy = true;
    const accepted: string[] = [];
    let error: string | undefined;
    try {
      for (const entry of entries as Entry[]) {
        // Navigation/disable may revoke the session while a durable ACK is in flight.
        try { this.get(context, token); }
        catch { error = '页面资源已失效；已接受的项目仍会保留，请重新查找剩余资源。'; break; }
        if (!entry.item.accepted) {
          try { await send(entry.item, entry.requestId); entry.item.accepted = true; }
          catch { error = '部分资源未发送，请检查桌面连接后重试剩余项。'; break; }
        }
        accepted.push(entry.item.id);
      }
      return {accepted, remaining: (entries as Entry[]).filter(entry => !entry.item.accepted).map(entry => entry.item.id), error};
    } finally { session.busy = false; }
  }
}
