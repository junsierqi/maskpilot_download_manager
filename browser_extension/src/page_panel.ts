// Injected as a classic isolated-world script: no imports or page messaging bridge.
(() => {
  const globals = globalThis as typeof globalThis & {
    browser?: typeof chrome; maskpilotDocument?: string; maskpilotPanel?: boolean;
  };
  if (globals.maskpilotPanel || !globals.maskpilotDocument) return;
  globals.maskpilotPanel = true;
  const api = globals.browser ?? chrome;
  const documentToken = globals.maskpilotDocument;
  interface Item {id: string; url: string; name: string; kind: string; accepted: boolean;}
  interface Model {token: string; items: Item[];}
  let model: Model | undefined, busy = false;
  const selected = new Set<string>();
  const host = document.createElement('aside');
  host.id = 'maskpilot-download-panel';
  host.setAttribute('aria-label', 'MaskPilot 页面下载资源');
  const shadow = host.attachShadow({mode: 'open'});
  const style = document.createElement('style');
  style.textContent = ':host{all:initial;position:fixed!important;right:18px!important;bottom:18px!important;z-index:2147483646!important;font:14px/1.45 system-ui,sans-serif;color:#16304d}' +
    '*{box-sizing:border-box}button{font:inherit;cursor:pointer;border:1px solid #c7d4e6;border-radius:9px;background:#fff;color:#16304d;padding:8px 12px}button:focus-visible,input:focus-visible{outline:3px solid #81aaff;outline-offset:2px}button:disabled{opacity:.55;cursor:default}' +
    '#panel-toggle,#panel-send{background:#285fe9;color:white;border-color:#285fe9}#panel-body{width:min(360px,calc(100vw - 36px));margin-bottom:8px;background:#fff;border:1px solid #d6dfed;border-radius:14px;box-shadow:0 10px 40px #17335b33;overflow:hidden}' +
    'header,footer{padding:12px;display:flex;align-items:center;gap:8px}header strong{flex:1}#panel-items{max-height:min(310px,45vh);overflow:auto;padding:0 12px}label{display:flex;gap:8px;border-top:1px solid #edf1f7;padding:10px 0;align-items:start}input{margin-top:4px}span{min-width:0;overflow-wrap:anywhere}small{display:block;color:#687e98;font-size:12px}#panel-status{margin:0;padding:0 12px 12px;color:#526783;white-space:pre-wrap}footer{justify-content:space-between;border-top:1px solid #edf1f7}[hidden]{display:none!important}';
  shadow.append(style);
  const body = document.createElement('section'); body.id = 'panel-body'; body.hidden = true;
  const header = document.createElement('header');
  const title = document.createElement('strong'); title.textContent = '页面下载资源';
  function button(id: string, text: string): HTMLButtonElement {
    const value = document.createElement('button'); value.id = id; value.textContent = text; value.type = 'button'; return value;
  }
  const close = button('panel-close', '关闭'); header.append(title, close);
  const list = document.createElement('div'); list.id = 'panel-items';
  const footer = document.createElement('footer');
  const refresh = button('panel-refresh', '重新查找'), send = button('panel-send', '下载所选');
  footer.append(refresh, send);
  const status = document.createElement('p'); status.id = 'panel-status'; status.setAttribute('role', 'status');
  body.append(header, list, status, footer);
  const toggle = button('panel-toggle', 'MaskPilot 下载'); toggle.setAttribute('aria-expanded', 'false');
  shadow.append(body, toggle); (document.documentElement || document.body).append(host);
  function render(): void {
    list.replaceChildren();
    for (const item of model?.items ?? []) {
      const row = document.createElement('label'); row.dataset.id = item.id;
      const check = document.createElement('input'); check.type = 'checkbox'; check.value = item.id;
      check.disabled = item.accepted || busy; check.setAttribute('aria-label', item.name || item.url);
      check.checked = selected.has(item.id) && !item.accepted;
      check.addEventListener('click', event => {
        // The open shadow DOM is page-mutable; its values/checked state grant no authority.
        if (!event.isTrusted || item.accepted || busy) { event.preventDefault(); return; }
        if (selected.has(item.id)) selected.delete(item.id); else selected.add(item.id);
        check.checked = selected.has(item.id);
      });
      const text = document.createElement('span'); text.textContent = item.name || '页面资源';
      const detail = document.createElement('small');
      try { const url = new URL(item.url); detail.textContent = url.host + url.pathname; } catch { detail.textContent = ''; }
      if (item.accepted) detail.textContent = '已发送 · ' + detail.textContent;
      text.append(detail); row.append(check, text); list.append(row);
    }
    if (!model?.items.length) { const empty = document.createElement('p'); empty.textContent = '没有发现可直接下载的资源。'; list.append(empty); }
    toggle.textContent = 'MaskPilot · ' + (model?.items.length ?? 0) + ' 项资源';
    refresh.disabled = send.disabled = busy;
  }
  function accept(value: unknown): void {
    if (!value || typeof value !== 'object') return;
    const candidate = value as Model;
    if (typeof candidate.token !== 'string' || !/^[a-f0-9]{32}$/.test(candidate.token) || !Array.isArray(candidate.items) || candidate.items.length > 200) return;
    if (candidate.items.some(item => !item || !/^[a-f0-9]{32}$/.test(item.id) || typeof item.name !== 'string' || typeof item.url !== 'string')) return;
    model = candidate;
    for (const id of selected) if (!model.items.some(item => item.id === id && !item.accepted)) selected.delete(id);
    host.hidden = false; render();
  }
  toggle.addEventListener('click', event => {
    if (!event.isTrusted) return;
    body.hidden = !body.hidden; toggle.setAttribute('aria-expanded', String(!body.hidden));
  });
  close.addEventListener('click', event => { if (event.isTrusted) host.hidden = true; });
  refresh.addEventListener('click', async event => {
    if (!event.isTrusted || busy) return;
    busy = true; render(); status.textContent = '正在查找…';
    try {
      const response = await api.runtime.sendMessage({kind: 'panel-refresh', document: documentToken});
      if (!response?.ok) throw new Error(response?.error || '无法重新查找。');
      accept(response.model); status.textContent = '仅发送你明确选择的资源。';
    } catch (error) { status.textContent = error instanceof Error ? error.message : '请重新打开页面面板。'; }
    finally { busy = false; render(); }
  });
  send.addEventListener('click', async event => {
    if (!event.isTrusted || busy || !model) return;
    const ids = model.items.filter(item => selected.has(item.id) && !item.accepted).map(item => item.id);
    if (!ids.length) { status.textContent = '请先选择资源。'; return; }
    const batch = model; busy = true; render(); status.textContent = '正在发送到桌面…';
    try {
      const response = await api.runtime.sendMessage({kind: 'panel-submit', document: documentToken, token: batch.token, ids});
      if (!response?.ok) throw new Error(response?.error || '资源发送失败。');
      if (model?.token === batch.token) {
        const accepted = new Set(Array.isArray(response.accepted) ? response.accepted : []);
        for (const item of model.items) if (accepted.has(item.id)) { item.accepted = true; selected.delete(item.id); }
        status.textContent = '已发送 ' + accepted.size + ' 项。' + (response.error || '');
      }
    } catch (error) { status.textContent = error instanceof Error ? error.message : '请重试剩余资源。'; }
    finally { busy = false; render(); }
  });
  api.runtime.onMessage.addListener((message, sender) => {
    if (sender.id !== api.runtime.id || sender.tab) return false;
    if (message?.kind === 'panel-model') accept(message.model);
    if (message?.kind === 'panel-hide') { host.hidden = true; model = undefined; selected.clear(); render(); }
    return false;
  });
})();
