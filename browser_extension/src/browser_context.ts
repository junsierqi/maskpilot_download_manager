import {safeHttpUrl} from './protocol.js';

export interface CookieContext { firefox: boolean; storeId?: string; incognito: boolean; }
interface TrustedSource {cookieStoreId?: string; incognito?: boolean;}
export function cookieContext(source: TrustedSource | undefined, firefox: boolean): CookieContext {
  return {firefox, storeId: source?.cookieStoreId, incognito: source?.incognito === true};
}
export interface CookieQuery {url: string; storeId?: string; partitionKey: Record<string, never>; firstPartyDomain?: null;}
export interface SourceCookie {
  name: string; value: string; firstPartyDomain?: string;
  partitionKey?: {topLevelSite?: string} | null;
}
export async function cookieHeader(url: string, context: CookieContext, enabled: boolean,
  read: (query: CookieQuery) => Promise<readonly SourceCookie[]>): Promise<string | undefined> {
  if (!enabled) return undefined;
  if (context.incognito || /private/i.test(context.storeId ?? ''))
    throw new Error('暂不转发隐私窗口中的登录信息，请保留浏览器下载。');
  if (context.firefox && (!context.storeId || !/^[\w-]{1,128}$/.test(context.storeId)))
    throw new Error('无法确认此下载的 Cookie 容器，请保留浏览器下载。');
  // Both supported browsers interpret an empty partition filter as ALL partitions.
  // Querying only unpartitioned cookies would miss an unsupported login context.
  const query: CookieQuery = {url: safeHttpUrl(url), partitionKey: {}};
  if (context.firefox) query.firstPartyDomain = null;
  if (context.storeId !== undefined) query.storeId = context.storeId;
  let cookies: readonly SourceCookie[];
  try { cookies = await read(query); }
  catch { throw new Error('无法安全读取此地址的登录信息，请保留浏览器下载。'); }
  if (cookies.length > 512) throw new Error('此地址的登录信息过多，请在浏览器中下载。');
  const values: string[] = [];
  for (const cookie of cookies) {
    // An exact URL alone does not identify the top-level partition of a third-party cookie.
    if (cookie.firstPartyDomain || cookie.partitionKey?.topLevelSite)
      throw new Error('暂不转发分区 Cookie，请保留浏览器下载或明确关闭登录信息转发。');
    if (!/^[!#$%&'*+.^_`|~0-9a-z-]+$/i.test(cookie.name) || /[\x00-\x1f\x7f;]/.test(cookie.value))
      throw new Error('此地址的登录信息格式不受支持。');
    values.push(`${cookie.name}=${cookie.value}`);
  }
  const header = values.join('; ');
  if (new TextEncoder().encode(header).length > 16_384) throw new Error('此地址的 Cookie 过长，请在浏览器中下载。');
  return header || undefined;
}
