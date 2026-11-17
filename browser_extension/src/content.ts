(() => {
  const found: {url: string; name: string; kind: 'file' | 'media'}[] = [];
  const seen = new Set<string>();
  function add(raw: string | null | undefined, name: string, kind: 'file' | 'media'): void {
    if (!raw || raw.length > 32_768 || found.length >= 200) return;
    try {
      const url = new URL(raw, document.baseURI);
      if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password) return;
      url.hash = '';
      if (seen.has(url.href)) return;
      seen.add(url.href);
      found.push({url: url.href, name: name.trim().slice(0, 180), kind});
    } catch { /* Unusable DOM values are omitted. */ }
  }
  for (const element of document.querySelectorAll<HTMLMediaElement>('video,audio')) {
    add(element.currentSrc || element.src, element.getAttribute('aria-label') || element.title || '页面媒体', 'media');
  }
  for (const source of document.querySelectorAll<HTMLSourceElement>('video source,audio source')) add(source.src, '媒体源', 'media');
  for (const anchor of document.querySelectorAll<HTMLAnchorElement>('a[href]')) {
    if (anchor.hasAttribute('download') || anchor.getAttribute('href')?.trim()) {
      add(anchor.href, anchor.download || anchor.textContent || '页面文件', /\.(mp4|mkv|webm|mp3|m4a|flac|m3u8|mpd)(?:[?#]|$)/i.test(anchor.href) ? 'media' : 'file');
    }
  }
  for (const entry of performance.getEntriesByType('resource')) {
    if (/\.(m3u8|mpd|mp4|webm|m4a|mp3)(?:[?#]|$)/i.test(entry.name)) add(entry.name, '已加载的媒体', 'media');
  }
  return found;
})();
