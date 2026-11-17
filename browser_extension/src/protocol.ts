export const HOST_NAME = 'com.maskpilot.downloads';
export const MAX_URL_LENGTH = 32_768;
export const MAX_CANDIDATES = 200;
export const REQUEST_TIMEOUT_MS = 12_000;
export const COMMANDS = ['ping', 'prepare', 'add', 'resume', 'pause', 'cancel', 'remove', 'list'] as const;
export type Command = typeof COMMANDS[number];
export interface NativeRequest {
  version: 1;
  requestId: string;
  command: Command;
  url?: string;
  filename?: string;
  taskId?: string;
  referrer?: string;
  cookie?: string;
  userAgent?: string;
  media?: boolean;
}
export interface NativeTask {
  id: string;
  name?: string;
  state?: string;
  downloaded?: number;
  total?: number;
}
export interface NativeResponse {
  version: 1;
  requestId: string;
  ok: boolean;
  taskId?: string;
  error?: string;
  tasks?: NativeTask[];
}
export interface Candidate { url: string; name: string; kind: 'file' | 'media'; }
const identifier = /^[a-f0-9]{32}$/;
const controls = /[\u0000-\u001f\u007f]/;
export function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}
export function safeHttpUrl(value: unknown): string {
  if (typeof value !== 'string' || value.length === 0 || value.length > MAX_URL_LENGTH || controls.test(value))
    throw new Error('下载地址无效或过长。');
  let url: URL;
  try { url = new URL(value); } catch { throw new Error('请输入完整的 HTTP 或 HTTPS 地址。'); }
  if (!['http:', 'https:'].includes(url.protocol) || !url.hostname || url.username || url.password)
    throw new Error('仅支持不含用户名与密码的 HTTP/HTTPS 地址。');
  url.hash = '';
  return url.href;
}
export function safeFilename(value: unknown): string | undefined {
  if (value === undefined || value === '') return undefined;
  if (typeof value !== 'string' || value.length > 1024 || controls.test(value)) throw new Error('文件名无效。');
  let name = value.split(/[\\/]/).at(-1)!.replace(/[<>:"|?*]/g, '_').replace(/[. ]+$/g, '').slice(0, 180);
  if (!name || name === '.' || name === '..') return undefined;
  if (/^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)/i.test(name)) name = '_' + name;
  return name;
}
export function newRequestId(): string {
  return Array.from(crypto.getRandomValues(new Uint8Array(16)), byte => byte.toString(16).padStart(2, '0')).join('');
}
export function validateRequest(input: unknown): NativeRequest {
  if (!isRecord(input) || input.version !== 1 || typeof input.requestId !== 'string' || !identifier.test(input.requestId)
      || !COMMANDS.includes(input.command as Command)) throw new Error('原生协议请求无效。');
  const command = input.command as Command;
  const result: NativeRequest = { version: 1, requestId: input.requestId, command };
  if (command === 'prepare' || command === 'add') result.url = safeHttpUrl(input.url);
  if (input.url !== undefined && result.url === undefined) throw new Error('该命令不接受下载地址。');
  if (['resume', 'pause', 'cancel', 'remove'].includes(command)) {
    if (typeof input.taskId !== 'string' || !identifier.test(input.taskId)) throw new Error('任务编号无效。');
    result.taskId = input.taskId;
  }
  if (input.filename !== undefined) result.filename = safeFilename(input.filename);
  if (input.referrer !== undefined && input.referrer !== '') result.referrer = safeHttpUrl(input.referrer);
  for (const [field, maximum] of [['cookie', 16_384], ['userAgent', 1024]] as const) {
    const value = input[field];
    if (value !== undefined) {
      if (typeof value !== 'string' || value.length > maximum || controls.test(value)) throw new Error('请求凭据或浏览器标识无效。');
      result[field] = value;
    }
  }
  if (input.media !== undefined) {
    if (typeof input.media !== 'boolean') throw new Error('媒体标记无效。');
    result.media = input.media;
  }
  return result;
}
export function validateResponse(input: unknown, expectedId: string): NativeResponse {
  if (!isRecord(input) || input.version !== 1 || input.requestId !== expectedId || typeof input.ok !== 'boolean')
    throw new Error('桌面响应与请求不匹配。');
  if (input.taskId !== undefined && (typeof input.taskId !== 'string' || !identifier.test(input.taskId)))
    throw new Error('桌面响应包含无效任务编号。');
  if (input.error !== undefined && (typeof input.error !== 'string' || input.error.length > 4096)) throw new Error('桌面错误消息无效。');
  if (input.tasks !== undefined && (!Array.isArray(input.tasks) || input.tasks.length > 1000)) throw new Error('任务列表过大或无效。');
  return input as unknown as NativeResponse;
}
export function normalizeCandidates(input: unknown): Candidate[] {
  if (!Array.isArray(input)) return [];
  const seen = new Set<string>();
  const output: Candidate[] = [];
  for (const item of input.slice(0, MAX_CANDIDATES * 4)) {
    if (!isRecord(item)) continue;
    try {
      const url = safeHttpUrl(item.url);
      if (seen.has(url)) continue;
      seen.add(url);
      output.push({url, name: typeof item.name === 'string' ? item.name.slice(0, 180) : '', kind: item.kind === 'media' ? 'media' : 'file'});
      if (output.length === MAX_CANDIDATES) break;
    } catch { /* An unsupported page URL is not a download candidate. */ }
  }
  return output;
}
export function withTimeout<T>(operation: Promise<T>, milliseconds = REQUEST_TIMEOUT_MS): Promise<T> {
  let timer: ReturnType<typeof setTimeout>;
  return Promise.race([operation, new Promise<never>((_, reject) => {
    timer = setTimeout(() => reject(new Error('连接桌面程序超时。原任务不会被静默丢弃。')), milliseconds);
  })]).finally(() => clearTimeout(timer));
}
export interface HandoffDependencies {
  pauseBrowser(): Promise<void>;
  prepareNative(): Promise<string>;
  cancelBrowser(): Promise<void>;
  resumeBrowser(): Promise<void>;
  cancelNative(taskId: string): Promise<void>;
  resumeNative(taskId: string): Promise<void>;
}
export interface HandoffResult {
  status: 'transferred' | 'browser-restored' | 'native-paused' | 'recovery-required';
  taskId?: string;
  error?: string;
}
function describe(error: unknown): string { return error instanceof Error ? error.message : '无法转交下载。'; }
export async function handoffDownload(deps: HandoffDependencies): Promise<HandoffResult> {
  try { await deps.pauseBrowser(); }
  catch (error) { return {status: 'browser-restored', error: describe(error)}; }
  let taskId: string;
  try {
    taskId = await deps.prepareNative();
    if (!identifier.test(taskId)) throw new Error('桌面程序没有确认已保存的任务。');
  } catch (error) {
    try { await deps.resumeBrowser(); return {status: 'browser-restored', error: describe(error)}; }
    catch { return {status: 'recovery-required', error: '未能恢复浏览器下载，请在浏览器下载列表中继续。'}; }
  }
  try { await deps.cancelBrowser(); }
  catch (error) {
    let nativeCancelled = true;
    try { await deps.cancelNative(taskId); } catch { nativeCancelled = false; }
    try {
      await deps.resumeBrowser();
      return {status: nativeCancelled ? 'browser-restored' : 'recovery-required', taskId,
        error: nativeCancelled ? describe(error) : '浏览器已恢复，桌面中仍有一个暂停任务，请手动取消。'};
    } catch { return {status: 'recovery-required', taskId, error: '请检查浏览器下载列表；桌面暂停任务已保留或取消。'}; }
  }
  try { await deps.resumeNative(taskId); return {status: 'transferred', taskId}; }
  catch { return {status: 'native-paused', taskId, error: '下载已安全保存到桌面程序。请在 MaskPilot 中点击开始。'}; }
}
