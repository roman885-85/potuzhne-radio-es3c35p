/*  ПОТУЖНЕ РАДІО — веб-сторінка.
 *
 *  Одна сторінка на все: сторінки-розділи перемикаються адресою після «#».
 *  Живий стан плеєра приходить websocket'ом yoRadio (/ws), усе дописане —
 *  JSON-ом з /api/state раз на дві секунди. Команди yoRadio йдуть у
 *  websocket, дописані — у /api/set.
 */
'use strict';

const VER = typeof yoVersion !== 'undefined' ? yoVersion : '';
const BUILD = typeof prBuild !== 'undefined' ? prBuild : '';

/* ---------------------------------------------------------------- утиліти */
const $ = (s, r = document) => r.querySelector(s);
/*  append() без «null» на сторінці: умовні шматки передаємо як є  */
const _append = Element.prototype.append;
Element.prototype.append = function (...k) { return _append.apply(this, k.flat(Infinity).filter(x => x != null && x !== false)); };
function h(tag, props, ...kids) {
  const el = document.createElement(tag);
  if (props) for (const k in props) {
    const v = props[k];
    if (v == null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'style' && typeof v === 'object') Object.assign(el.style, v);
    else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2), v);
    else if (k === 'html') el.innerHTML = v;
    else if (k === 'value' || k === 'checked' || k === 'disabled' || k === 'selected') el[k] = v;
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const c of kids.flat(Infinity)) {
    if (c == null || c === false) continue;
    el.append(c.nodeType ? c : document.createTextNode(String(c)));
  }
  return el;
}
const P = {
  play: '<path d="M7 4.5v15l12-7.5z" fill="currentColor" stroke="none"/>',
  pause: '<rect x="6" y="4.5" width="4" height="15" rx="1" fill="currentColor" stroke="none"/><rect x="14" y="4.5" width="4" height="15" rx="1" fill="currentColor" stroke="none"/>',
  stop: '<rect x="6" y="6" width="12" height="12" rx="1.5" fill="currentColor" stroke="none"/>',
  prev: '<path d="M18 5v14L8 12z" fill="currentColor" stroke="none"/><path d="M6 5v14"/>',
  next: '<path d="M6 5v14l10-7z" fill="currentColor" stroke="none"/><path d="M18 5v14"/>',
  vol: '<path d="M4 9v6h4l5 4V5L8 9z"/><path d="M16.5 8.5a5 5 0 0 1 0 7M19 6a8.5 8.5 0 0 1 0 12"/>',
  vol0: '<path d="M4 9v6h4l5 4V5L8 9z"/><path d="M17 9l5 5M22 9l-5 5"/>',
  radio: '<circle cx="12" cy="11" r="2"/><path d="M8.5 7.5a5 5 0 0 0 0 7M15.5 7.5a5 5 0 0 1 0 7M5.6 4.6a9 9 0 0 0 0 12.8M18.4 4.6a9 9 0 0 1 0 12.8M12 13v8"/>',
  sd: '<path d="M7 3h8l4 4v13a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z"/><path d="M10 3v4M13 3v4M16 4v3"/>',
  book: '<path d="M4 5a2 2 0 0 1 2-2h13v16H6a2 2 0 0 0-2 2z"/><path d="M4 21V5M11.5 6.5v7M9 9h5"/>',
  star: '<path d="M12 3.5l2.6 5.3 5.9.9-4.3 4.1 1 5.8-5.2-2.7-5.2 2.7 1-5.8-4.3-4.1 5.9-.9z"/>',
  starf: '<path d="M12 3.5l2.6 5.3 5.9.9-4.3 4.1 1 5.8-5.2-2.7-5.2 2.7 1-5.8-4.3-4.1 5.9-.9z" fill="currentColor"/>',
  edit: '<path d="M4 20h4L19 9l-4-4L4 16z"/><path d="M13.5 6.5l4 4"/>',
  trash: '<path d="M4 7h16M9 7V4h6v3M6 7l1 13h10l1-13M10 11v6M14 11v6"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  grip: '<circle cx="9" cy="6" r="1.2" fill="currentColor"/><circle cx="15" cy="6" r="1.2" fill="currentColor"/><circle cx="9" cy="12" r="1.2" fill="currentColor"/><circle cx="15" cy="12" r="1.2" fill="currentColor"/><circle cx="9" cy="18" r="1.2" fill="currentColor"/><circle cx="15" cy="18" r="1.2" fill="currentColor"/>',
  search: '<circle cx="11" cy="11" r="6.5"/><path d="M16 16l4.5 4.5"/>',
  up: '<path d="M12 19V5M6 11l6-6 6 6"/>',
  down: '<path d="M12 5v14M6 13l6 6 6-6"/>',
  upload: '<path d="M12 16V4M7 9l5-5 5 5M4 16v3a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-3"/>',
  download: '<path d="M12 4v12M7 11l5 5 5-5M4 16v3a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-3"/>',
  wifi: '<path d="M2.5 9a14 14 0 0 1 19 0M5.5 12.5a9.5 9.5 0 0 1 13 0M8.5 16a5 5 0 0 1 7 0"/><circle cx="12" cy="19.2" r=".9" fill="currentColor"/>',
  lock: '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/>',
  alarm: '<circle cx="12" cy="13" r="7.5"/><path d="M12 9v4l2.5 2M4 4.5L6.5 2.5M20 4.5l-2.5-2"/>',
  screen: '<rect x="3" y="4" width="18" height="13" rx="2"/><path d="M8 21h8M12 17v4"/>',
  mic: '<rect x="9" y="3" width="6" height="11" rx="3"/><path d="M5.5 11a6.5 6.5 0 0 0 13 0M12 17.5V21M8.5 21h7"/>',
  eq: '<path d="M5 20v-6M5 10V4M12 20v-9M12 7V4M19 20v-4M19 12V4M3 14h4M10 7h4M17 16h4"/>',
  gear: '<circle cx="12" cy="12" r="3"/><path d="M12 2.5v3M12 18.5v3M4.2 5.6l2.1 2.1M17.7 16.3l2.1 2.1M2.5 12h3M18.5 12h3M4.2 18.4l2.1-2.1M17.7 7.7l2.1-2.1"/>',
  code: '<path d="M8 7l-5 5 5 5M16 7l5 5-5 5M13.5 4l-3 16"/>',
  refresh: '<path d="M20 11a8 8 0 0 0-14.3-4.5M4 13a8 8 0 0 0 14.3 4.5"/><path d="M5 3v4h4M19 21v-4h-4"/>',
  info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7.5v.5"/>',
  menu: '<path d="M4 7h16M4 12h16M4 17h16"/>',
  x: '<path d="M6 6l12 12M18 6L6 18"/>',
  check: '<path d="M5 12.5l4.5 4.5L19 7.5"/>',
  rec: '<circle cx="12" cy="12" r="6" fill="currentColor" stroke="none"/>',
  globe: '<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"/>',
  moon: '<path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"/>',
  chip: '<rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2.5v3M15 2.5v3M9 18.5v3M15 18.5v3M2.5 9h3M2.5 15h3M18.5 9h3M18.5 15h3"/>',
  clock: '<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/>',
  image: '<rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="9" cy="10" r="2"/><path d="M21 16l-5-5-9 9"/>',
  headph: '<path d="M4 15v-3a8 8 0 0 1 16 0v3"/><rect x="3" y="14" width="4" height="7" rx="1.5"/><rect x="17" y="14" width="4" height="7" rx="1.5"/>',
  shuffle: '<path d="M3 7h3.5c2 0 3.2 1 4.5 3l2 4c1.3 2 2.5 3 4.5 3H21M3 17h3.5c1.6 0 2.7-.6 3.7-1.8M14 8.8c1-1.2 2.1-1.8 3.5-1.8H21M18 4l3 3-3 3M18 14l3 3-3 3"/>',
  file: '<path d="M7 3h7l5 5v12a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1z"/><path d="M14 3v5h5"/>',
  bell: '<path d="M6 17V11a6 6 0 0 1 12 0v6l1.5 2h-15z"/><path d="M10 21h4"/>',
  speak: '<path d="M4 5h16v11H9.5L5 19.5V16H4z"/><path d="M9 9.5v2M12 8v5M15 9.5v2"/>',
};
function ic(name, cls) {
  const s = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  s.setAttribute('viewBox', '0 0 24 24'); s.setAttribute('fill', 'none'); s.setAttribute('stroke', 'currentColor');
  s.setAttribute('stroke-width', '2'); s.setAttribute('stroke-linecap', 'round'); s.setAttribute('stroke-linejoin', 'round');
  s.setAttribute('aria-hidden', 'true');
  if (cls) s.setAttribute('class', cls);
  s.innerHTML = P[name] || '';
  return s;
}
const pad2 = n => String(n).padStart(2, '0');
function mmss(s) { s = Math.max(0, Math.floor(s || 0)); const hh = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, ss = s % 60; return hh ? `${hh}:${pad2(m)}:${pad2(ss)}` : `${m}:${pad2(ss)}`; }
function size(b) { if (b < 1024) return b + ' Б'; if (b < 1048576) return (b / 1024).toFixed(0) + ' КБ'; if (b < 1073741824) return (b / 1048576).toFixed(1).replace('.', ',') + ' МБ'; return (b / 1073741824).toFixed(1).replace('.', ',') + ' ГБ'; }
function plural(n, one, few, many) { const a = Math.abs(n) % 100, b = a % 10; if (a > 10 && a < 20) return many; if (b > 1 && b < 5) return few; if (b === 1) return one; return many; }
const bytes = s => new TextEncoder().encode(s).length;

let toastT = 0;
function toast(msg, bad) {
  let t = $('.toast');
  if (!t) { t = h('div', { class: 'toast', role: 'status' }); document.body.append(t); }
  t.textContent = msg; t.classList.toggle('bad', !!bad); t.hidden = false;
  clearTimeout(toastT); toastT = setTimeout(() => { t.hidden = true; }, bad ? 5000 : 2600);
}

function fillRange(r) { const mn = +r.min || 0, mx = +r.max || 100; r.style.setProperty('--p', ((r.value - mn) / (mx - mn) * 100) + '%'); }
/*  Чи людина зараз крутить цей елемент. На дотику браузер часто НЕ ставить
    фокус на повзунок, тому самого document.activeElement мало: стан, що
    прийшов з радіо (ще старий, бо зміну ми шлемо із затримкою), затирав те,
    що тягнуть пальцем — повзунок смикався назад. Тому запам'ятовуємо мить
    останньої дії й ще секунду елемент не чіпаємо.  */
const HELD = new WeakMap();
function hold(el, ms) { if (el) HELD.set(el, Date.now() + (ms || 1200)); }
function held(el) {
  if (!el) return false;
  if (document.activeElement === el) return true;
  const t = HELD.get(el);
  return !!t && Date.now() < t;
}
['pointerdown', 'touchstart', 'input', 'change'].forEach(ev =>
  document.addEventListener(ev, e => {
    const t = e.target;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'SELECT')) hold(t);
  }, true));

function range(min, max, val, oninput, onchange, step) {
  const r = h('input', { type: 'range', min, max, step: step || 1, value: val });
  fillRange(r);
  r.addEventListener('input', () => { fillRange(r); oninput && oninput(+r.value, r); });
  if (onchange) r.addEventListener('change', () => onchange(+r.value, r));
  return r;
}
function sw(checked, onchange) {
  const i = h('input', { type: 'checkbox', checked: !!checked });
  i.addEventListener('change', () => onchange(i.checked, i));
  return h('label', { class: 'sw' }, i, h('i'));
}
function seg(items, cur, onpick) {
  const el = h('div', { class: 'seg', role: 'group' });
  items.forEach(([v, label]) => {
    const b = h('button', { type: 'button', class: v === cur ? 'on' : '' }, label);
    b.onclick = () => { [...el.children].forEach(x => x.classList.remove('on')); b.classList.add('on'); onpick(v); };
    b.dataset.v = v;
    el.append(b);
  });
  el.set = v => [...el.children].forEach(x => x.classList.toggle('on', x.dataset.v == String(v)));
  return el;
}
function row(title, sub, ctl) { return h('div', { class: 'row' }, h('div', { class: 'lbl' }, h('b', null, title), sub ? h('small', null, sub) : null), ctl); }
/*  Вибір файлу своєю кнопкою: напис рідного поля залежить від мови браузера  */
function filePick(accept, multiple, label) {
  const inp = h('input', { type: 'file', accept, multiple, hidden: true });
  const name = h('span', { class: 'note', style: { margin: 0, wordBreak: 'break-all' } }, 'файл не вибрано');
  inp.addEventListener('change', () => { const n = [...inp.files].map(f => f.name); name.textContent = n.length ? n.join(', ') : 'файл не вибрано'; });
  const el = h('div', { class: 'bar fld' }, btn(label || 'Вибрати файл', 'file', () => inp.click()), name, inp);
  el.input = inp;
  return el;
}
function card(title, ...kids) { return h('section', { class: 'card' }, title ? h('h2', null, title) : null, ...kids); }
function btn(label, icon, onclick, cls) { return h('button', { type: 'button', class: 'btn ' + (cls || ''), onclick }, icon ? ic(icon) : null, label); }
function ibtn(icon, title, onclick, cls) { return h('button', { type: 'button', class: 'btn ico ' + (cls || ''), title, 'aria-label': title, onclick }, ic(icon)); }

function dialog(title, body, buttons, opts) {
  const box = h('div', { class: 'box', role: 'dialog', 'aria-modal': 'true', 'aria-label': title });
  const d = h('div', { class: 'dlg' }, box);
  const close = () => { d.remove(); document.removeEventListener('keydown', esc); opts && opts.onclose && opts.onclose(); };
  const esc = e => { if (e.key === 'Escape') close(); };
  document.addEventListener('keydown', esc);
  d.addEventListener('mousedown', e => { if (e.target === d) close(); });
  box.append(h('h3', null, title), body, h('div', { class: 'foot' }, buttons.map(b => b === '|' ? h('span', { class: 'sp' }) : b)));
  document.body.append(d);
  const f = box.querySelector('input:not([type=checkbox]),select,textarea'); if (f) setTimeout(() => f.focus(), 30);
  d.close = close;
  return d;
}
function confirmBox(title, text, okLabel, danger) {
  return new Promise(res => {
    let done = false;
    const d = dialog(title, h('p', { style: { color: 'var(--dim)', margin: 0 } }, text), [
      btn('Скасувати', null, () => { done = true; d.close(); res(false); }, 'ghost'),
      btn(okLabel || 'Так', null, () => { done = true; d.close(); res(true); }, danger ? 'acc bad' : 'acc'),
    ], { onclose: () => { if (!done) res(false); } });
  });
}
function download(name, text, type) {
  const a = h('a', { href: URL.createObjectURL(new Blob([text], { type: type || 'text/plain' })), download: name });
  document.body.append(a); a.click(); setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
}

/* ---------------------------------------------------------------- зв'язок */
async function api(path, opts) {
  const r = await fetch(path, Object.assign({ cache: 'no-store' }, opts));
  const t = await r.text();
  let j = {};
  try { j = t ? JSON.parse(t) : {}; } catch (e) { j = { raw: t }; }
  if (!r.ok) throw new Error(j.err || ('помилка ' + r.status));
  return j;
}
function setx(obj) {
  const b = new URLSearchParams();
  for (const k in obj) b.append(k, obj[k]);
  return api('/api/set', { method: 'POST', body: b }).catch(e => toast('Радіо не прийняло команду: ' + e.message, true));
}

/*  Стан: W — websocket yoRadio, S — /api/state, C — налаштування yoRadio  */
const W = { name: '', title: '', vol: 0, playing: false, cur: 0, bitrate: 0, fmt: '', mode: 0, bass: 0, middle: 0, trebble: 0, balance: 0, sdpos: 0, sdmin: 0, sdmax: 0, sdtpos: 0, sdtend: 0, snuffle: 0 };
let S = null;
const C = {};
let ws = null, wsOk = false;
const wsQueue = [];

function wsConnect() {
  try { ws = new WebSocket(`ws://${location.host}/ws`); } catch (e) { setTimeout(wsConnect, 3000); return; }
  ws.onopen = () => { wsOk = true; ws.send('getindex=1'); while (wsQueue.length) ws.send(wsQueue.shift()); topStat(); };
  ws.onclose = () => { wsOk = false; topStat(); setTimeout(wsConnect, 2500); };
  ws.onerror = () => { try { ws.close(); } catch (e) {} };
  ws.onmessage = e => onWs(e.data);
}
function send(cmd) {
  if (ws && ws.readyState === 1) ws.send(cmd);
  else { wsQueue.push(cmd); if (wsQueue.length > 20) wsQueue.shift(); }
}
/*  Назву пісні плата шле як є — лапки всередині ламають JSON. Лагодимо.  */
function wsParse(t) {
  try { return JSON.parse(t); } catch (e) {}
  const m = t.match(/^\{"payload":\[\{"id":"(\w+)",\s*"value":\s*"([\s\S]*)"\}\]\}$/);
  if (m) return { payload: [{ id: m[1], value: m[2].replace(/\\(?!["\\/bfnrtu])/g, '\\\\') }] };
  return null;
}
function onWs(t) {
  const d = wsParse(t);
  if (!d) return;
  let what = 'misc';
  if (d.payload) {
    for (const { id, value } of d.payload) {
      if (id === 'nameset') { W.name = value; what = 'np'; }
      else if (id === 'meta') { W.title = value; what = 'np'; }
      else if (id === 'volume') { W.vol = +value; what = 'vol'; }
      else if (id === 'playerwrap') { W.playing = value === 'playing'; what = 'np'; }
      else if (id === 'bitrate') { W.bitrate = +value; what = 'np'; }
      else if (id === 'fmt') { W.fmt = value; what = 'np'; }
      else if (id in W) { W[id] = +value; what = 'eq'; }
    }
  } else if ('current' in d) { W.cur = +d.current; what = 'cur'; }
  else if ('file' in d) { what = 'playlist'; }
  else if ('playermode' in d) { W.mode = d.playermode === 'modesd' ? 1 : 0; what = 'mode'; }
  else if ('sdpos' in d) { Object.assign(W, { sdpos: +d.sdpos, sdtpos: +d.sdtpos, sdtend: +d.sdtend }); what = 'sd'; }
  else if ('sdmin' in d) { W.sdmin = +d.sdmin; W.sdmax = +d.sdmax; what = 'sd'; }
  else if ('snuffle' in d) { W.snuffle = +d.snuffle; what = 'sd'; }
  else if ('pong' in d || 'act' in d || 'dspontrue' in d) return;
  else { Object.assign(C, d); what = 'cfg'; }
  bus(what);
}

const listeners = new Set();
function bus(what) {
  if (what === 'playlist' || what === 'mode') loadPlayList();
  miniUpdate(); topStat();
  for (const f of listeners) try { f(what); } catch (e) { console.error(e); }
}

let stT = 0, stFails = 0, webStamp = 0, webWarned = false;
async function pollState() {
  clearTimeout(stT);
  try {
    S = await api('/api/state'); stFails = 0;
    /*  на радіо залили нові файли сторінки — перезавантажитись самим,
        щоб ніде не лишалось старої сторінки (незбережений список не губимо)  */
    if (S.web) {
      if (!webStamp) webStamp = S.web;
      else if (S.web !== webStamp) { if (!edDirty()) { location.reload(); return; } if (!webWarned) { webWarned = true; toast('Сторінку на радіо оновлено. Збережіть список станцій і оновіть сторінку.', true); } }
    }
    W.mode = S.mode;
    if (!wsOk) { W.name = S.name; W.title = S.title; W.vol = S.vol; W.playing = !!S.play; W.cur = S.idx; }
    if (S.logo !== LOGO.ver && LOGO.ver !== -1) loadLogos();
    if (S.msg) { toast(S.msg, true); setx({ msgClr: 1 }); }
    bus('state');
    otaBar();
  } catch (e) { stFails++; if (stFails === 3) toast('Радіо не відповідає', true); topStat(); }
  stT = setTimeout(pollState, document.hidden ? 8000 : 2000);
}
document.addEventListener('visibilitychange', () => { if (!document.hidden) pollState(); });

/* ---------------------------------------------------------------- логотипи */
const CRC_T = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
function crc32(str) { let c = 0xFFFFFFFF; for (const b of new TextEncoder().encode(str)) c = CRC_T[(c ^ b) & 255] ^ (c >>> 8); return ((c ^ 0xFFFFFFFF) >>> 0).toString(16).padStart(8, '0'); }
const PAL = [0x3A8D, 0x5A4B, 0x2C6A, 0x6A28, 0x2B0F, 0x7A6C, 0x4B09, 0x31CC].map(v => `rgb(${((v >> 11) & 31) * 255 / 31 | 0},${((v >> 5) & 63) * 255 / 63 | 0},${(v & 31) * 255 / 31 | 0})`);
const LOGO = { ver: -1, have: new Set(), url: new Map(), q: [], busy: 0 };
async function loadLogos() {
  try { const j = await api('/api/logos'); const changed = j.v !== LOGO.ver; LOGO.ver = j.v; LOGO.have = new Set(j.f); if (changed) { LOGO.url.clear(); bus('logos'); } } catch (e) {}
}
function initials(name) {
  const w = (name || '').split(/\s+/).map(x => x.replace(/^[^\p{L}\p{N}]+/u, '')).filter(Boolean);
  return w.slice(0, 2).map(x => [...x][0]).join('').toUpperCase() || '?';
}
function logoEl(name, url, cls) {
  const crc = crc32(url || '');
  const el = h('div', { class: 'logo ' + (cls || '') });
  const put = src => { el.textContent = ''; el.style.background = '#fff'; el.append(h('img', { src, alt: '' })); };
  const tile = () => { el.style.background = PAL[parseInt(crc.slice(-1), 16) & 7]; el.textContent = initials(name); };
  if (LOGO.url.has(crc)) { put(LOGO.url.get(crc)); return el; }
  tile();
  if (LOGO.have.has(crc + '.565')) {
    LOGO.q.push({ crc, cb: put }); logoPump();
  } else if (LOGO.have.has(crc + '.jpg')) {
    const u = `/logo/${crc}.jpg?v=${LOGO.ver}`; LOGO.url.set(crc, u); put(u);
  }
  return el;
}
function logoPump() {
  while (LOGO.busy < 3 && LOGO.q.length) {
    const { crc, cb } = LOGO.q.shift();
    if (LOGO.url.has(crc)) { cb(LOGO.url.get(crc)); continue; }
    LOGO.busy++;
    fetch(`/logo/${crc}.565?v=${LOGO.ver}`).then(r => r.ok ? r.arrayBuffer() : null).then(buf => {
      if (!buf || buf.byteLength !== 45 * 45 * 2) return;
      const cv = h('canvas', { width: 45, height: 45 }), cx = cv.getContext('2d'), im = cx.createImageData(45, 45), dv = new DataView(buf);
      for (let i = 0; i < 2025; i++) { const v = dv.getUint16(i * 2, true); im.data[i * 4] = ((v >> 11) & 31) * 255 / 31; im.data[i * 4 + 1] = ((v >> 5) & 63) * 255 / 63; im.data[i * 4 + 2] = (v & 31) * 255 / 31; im.data[i * 4 + 3] = 255; }
      cx.putImageData(im, 0, 0);
      const u = cv.toDataURL(); LOGO.url.set(crc, u); cb(u);
    }).catch(() => {}).finally(() => { LOGO.busy--; logoPump(); });
  }
}
/*  Свій логотип: картинку з комп'ютера зменшуємо до 45x45 тут, у браузері,
    і шлемо плати готові пікселі — тими ж, що вона зберігає сама.  */
function uploadLogo(url, file) {
  return new Promise((res, rej) => {
    const img = new Image();
    img.onload = async () => {
      const cv = h('canvas', { width: 45, height: 45 }), cx = cv.getContext('2d');
      cx.fillStyle = '#fff'; cx.fillRect(0, 0, 45, 45);
      const k = Math.min(45 / img.width, 45 / img.height), w = img.width * k, hh = img.height * k;
      cx.imageSmoothingQuality = 'high';
      cx.drawImage(img, (45 - w) / 2, (45 - hh) / 2, w, hh);
      const d = cx.getImageData(0, 0, 45, 45).data, out = new DataView(new ArrayBuffer(4050));
      for (let i = 0; i < 2025; i++) out.setUint16(i * 2, ((d[i * 4] & 0xF8) << 8) | ((d[i * 4 + 1] & 0xFC) << 3) | (d[i * 4 + 2] >> 3), true);
      const fd = new FormData(); fd.append('logo', new Blob([out.buffer]), crc32(url) + '.565');
      try { await api('/api/logo', { method: 'POST', body: fd }); await loadLogos(); res(); } catch (e) { rej(e); }
      URL.revokeObjectURL(img.src);
    };
    img.onerror = () => rej(new Error('це не картинка'));
    img.src = URL.createObjectURL(file);
  });
}

/* ---------------------------------------------------------------- станції */
/*  PL — те, що бачить плеєр (у режимі картки — треки картки);
    ED — список радіостанцій у редакторі, зі своїми незбереженими змінами.  */
let PL = [];
const ED = { list: [], orig: '', loaded: false };
function parseCSVText(t) {
  const out = [];
  for (let line of t.replace(/^﻿/, '').split(/\r?\n/)) {
    if (!line.trim()) continue;
    const p = line.split('\t');
    if (p.length >= 2) out.push({ name: p[0].trim(), url: p[1].trim(), ovol: parseInt(p[2]) || 0 });
  }
  return out;
}
const serialize = l => l.map(s => `${s.name}\t${s.url}\t${s.ovol | 0}`).join('\n') + (l.length ? '\n' : '');
const edDirty = () => ED.loaded && serialize(ED.list) !== ED.orig;
async function loadPlayList() {
  try { const t = await (await fetch('/data/playlist.csv', { cache: 'no-store' })).text(); PL = parseCSVText(t); bus('pl'); } catch (e) {}
}
async function loadEditor(force) {
  if (ED.loaded && !force) return;
  const t = await (await fetch('/api/stations', { cache: 'no-store' })).text();
  ED.list = parseCSVText(t); ED.orig = serialize(ED.list); ED.loaded = true;
}
window.addEventListener('beforeunload', e => { if (edDirty()) { e.preventDefault(); e.returnValue = ''; } });

/*  Імпорт: CSV плати, M3U/M3U8, PLS, JSON старих версій, просто адреси.  */
function parseAny(text, fname) {
  text = text.replace(/^﻿/, '');
  const out = [], isUrl = u => /^https?:\/\/\S+$/i.test(u);
  const nameFromUrl = u => { try { return new URL(u).hostname.replace(/^www\./, ''); } catch (e) { return u; } };
  const t = text.trim();
  if (/^\[playlist\]/i.test(t) || /\.pls$/i.test(fname)) {
    const f = {}, ti = {};
    t.split(/\r?\n/).forEach(l => { let m = l.match(/^File(\d+)=(.+)$/i); if (m) f[m[1]] = m[2].trim(); m = l.match(/^Title(\d+)=(.+)$/i); if (m) ti[m[1]] = m[2].trim(); });
    Object.keys(f).sort((a, b) => a - b).forEach(k => { if (isUrl(f[k])) out.push({ name: ti[k] || nameFromUrl(f[k]), url: f[k], ovol: 0 }); });
    return out;
  }
  if (/^\[\s*\{/.test(t) || /^\{/.test(t)) {
    let arr = [];
    try { const j = JSON.parse(t); arr = Array.isArray(j) ? j : [j]; }
    catch (e) { t.split(/\r?\n/).forEach(l => { try { arr.push(JSON.parse(l)); } catch (e2) {} }); }
    for (const o of arr) { const u = (o.url_resolved || o.url || '').trim(); if (isUrl(u)) out.push({ name: (o.name || nameFromUrl(u)).trim(), url: u, ovol: parseInt(o.ovol) || 0 }); }
    return out;
  }
  if (/#EXTM3U|#EXTINF/i.test(t) || /\.m3u8?$/i.test(fname)) {
    let nm = '';
    t.split(/\r?\n/).forEach(l => {
      l = l.trim();
      if (/^#EXTINF/i.test(l)) { nm = l.replace(/^#EXTINF:[^,]*,/i, '').trim(); }
      else if (isUrl(l)) { out.push({ name: nm || nameFromUrl(l), url: l, ovol: 0 }); nm = ''; }
    });
    return out;
  }
  t.split(/\r?\n/).forEach(l => {
    l = l.trim(); if (!l || l[0] === '#') return;
    let p = l.split('\t');
    if (p.length >= 2 && isUrl(p[1].trim())) { out.push({ name: p[0].trim() || nameFromUrl(p[1].trim()), url: p[1].trim(), ovol: parseInt(p[2]) || 0 }); return; }
    const m = l.match(/^(.*?)[;,|]\s*(https?:\/\/[^\s;,|]+)\s*(?:[;,|]\s*(-?\d+))?\s*$/i);
    if (m) { out.push({ name: m[1].trim().replace(/^"|"$/g, '') || nameFromUrl(m[2]), url: m[2], ovol: parseInt(m[3]) || 0 }); return; }
    if (isUrl(l)) out.push({ name: nameFromUrl(l), url: l, ovol: 0 });
  });
  return out;
}
function cleanSt(s) {
  let name = (s.name || '').replace(/[\t\r\n]+/g, ' ').trim(), url = (s.url || '').replace(/\s+/g, '');
  while (bytes(name) > 160) name = [...name].slice(0, -1).join('');
  return { name, url, ovol: Math.max(-64, Math.min(64, parseInt(s.ovol) || 0)) };
}

/* ---------------------------------------------------------------- каркас */
const PAGES = [
  { id: 'player', t: 'Плеєр', d: 'що грає, гучність, керування', i: 'play' },
  { id: 'stations', t: 'Станції', d: 'список, додати, змінити, імпорт і експорт', i: 'radio' },
  { id: 'find', t: 'Пошук станцій', d: 'вільні каталоги: назва, країна, мова, жанр', i: 'search' },
  { id: 'fav', t: 'Обране', d: 'шість швидких кнопок', i: 'star' },
  { id: 'sermons', t: 'Проповіді', d: 'архів проповідей із сайту церкви', i: 'book' },
  { id: 'records', t: 'Запис і картка', d: 'запис ефіру, файли на картці', i: 'sd' },
  { id: 'voice', t: 'Голосові команди', d: 'що сказати радіо в програмі', i: 'speak' },
  { g: 'Налаштування' },
  { id: 'alarm', t: 'Будильник і сон', d: 'розбудити й приспати', i: 'alarm' },
  { id: 'screen', t: 'Екран', d: 'яскравість, ніч, економія, світлодіод', i: 'screen' },
  { id: 'sound', t: 'Звук', d: 'еквалайзер на 10 смуг, обробка, під кімнату', i: 'eq' },
  { id: 'mic', t: 'Мікрофон', d: 'хлопки й стук, таймер сну, присутність', i: 'mic' },
  { id: 'wifi', t: 'Wi-Fi', d: 'мережі, пошук, підключення', i: 'wifi' },
  { id: 'time', t: 'Час і погода', d: 'часовий пояс, сервери, погода', i: 'clock' },
  { id: 'system', t: 'Система', d: 'поведінка радіо, перезавантаження', i: 'gear' },
  { id: 'dev', t: 'Розробник', d: 'батарея, IP, картка, аудіовихід, заставка, звуки подій', i: 'code' },
  { id: 'update', t: 'Оновлення', d: 'прошивка й файли сторінки', i: 'upload' },
  { id: 'about', t: 'Про пристрій', d: 'версія, пам\'ять, мережа, батарея', i: 'info' },
];
let curPage = '', pageLive = null;

function shell() {
  const nav = h('nav', { class: 'nav', 'aria-label': 'Розділи' });
  for (const p of PAGES) {
    if (p.g) { nav.append(h('div', { class: 'grp' }, p.g)); continue; }
    nav.append(h('a', { href: '#/' + p.id, 'data-id': p.id }, ic(p.i), h('span', null, p.t), h('small', null, p.d)));
  }
  const logo = h('span', { html: '<svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="#e6d25a" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="11" r="2"/><path d="M8.5 7.5a5 5 0 0 0 0 7M15.5 7.5a5 5 0 0 1 0 7M5.6 4.6a9 9 0 0 0 0 12.8M18.4 4.6a9 9 0 0 1 0 12.8M12 13v8"/></svg>' });
  const app = h('div', { class: 'shell', id: 'shell' },
    h('aside', { class: 'side' }, h('div', { class: 'brand' }, logo, h('div', null, h('b', null, 'ПОТУЖНЕ РАДІО'), h('small', null, VER ? `версія ${VER}${BUILD ? ' · ' + BUILD.slice(0, 10) : ''}` : ''))), nav),
    h('div', { class: 'scrim', onclick: () => $('#shell').classList.remove('open') }),
    h('div', { class: 'main' },
      h('header', { class: 'top' },
        h('button', { class: 'burger', type: 'button', 'aria-label': 'Розділи', onclick: () => $('#shell').classList.toggle('open') }, ic('menu')),
        h('h1', { id: 'ptitle' }, ''),
        h('button', { class: 'voicebtn', id: 'voicebtn', type: 'button', hidden: true, title: 'Голосова команда', 'aria-label': 'Голосова команда', onclick: () => voiceStart() }, ic('mic')),
        h('div', { class: 'stat', id: 'stat' })),
      h('div', { id: 'otabar', class: 'otabar', hidden: true }),
      h('div', { id: 'page' })),
    mini());
  $('#app').replaceWith(app);
}

function route() {
  let id = (location.hash.match(/^#\/(\w+)/) || [])[1];
  if (!id) id = location.pathname.includes('update') ? 'update' : location.pathname.includes('settings') ? 'wifi' : 'player';
  if (id === 'sounds') id = 'dev';                 /* звуки й заставка тепер у розробнику */
  const p = PAGES.find(x => x.id === id) || PAGES[0];
  curPage = p.id;
  document.querySelectorAll('.nav a').forEach(a => a.classList.toggle('on', a.dataset.id === p.id));
  $('#ptitle').textContent = p.t;
  document.title = p.t + ' · ПОТУЖНЕ РАДІО';
  $('#shell').classList.remove('open');
  if (pageLive) listeners.delete(pageLive);
  pageLive = null;
  const root = $('#page');
  root.textContent = '';
  const page = h('div', { class: 'page' });
  root.append(page);
  (VIEWS[p.id] || VIEWS.player)(page);
  window.scrollTo(0, 0);
}
function live(fn) { pageLive = fn; listeners.add(fn); fn('init'); }

/* ---------------------------------------------------------------- шапка */
function batSvg(pct, chg) {
  const col = pct < 10 ? '#ef5b5b' : pct < 25 ? '#f0a53a' : pct < 50 ? '#e6d25a' : '#5ccf6a';
  const w = Math.max(1, Math.round(18 * Math.min(100, Math.max(0, pct)) / 100));
  return `<svg viewBox="0 0 26 14"><rect x="1" y="1" width="21" height="12" rx="2.5" fill="none" stroke="#8c8c8c" stroke-width="1.5"/><rect x="23" y="4.5" width="2" height="5" rx="1" fill="#8c8c8c"/><rect x="2.8" y="2.8" width="${w}" height="8.4" rx="1.2" fill="${col}"/>${chg ? '<path d="M12.5 2.5L8.5 7.6h3l-1 4 4-5.1h-3z" fill="#fff" stroke="#000" stroke-width=".6"/>' : ''}</svg>`;
}
function sigSvg(rssi) {
  const n = !rssi ? 0 : rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
  let s = '<svg viewBox="0 0 18 14">';
  for (let i = 0; i < 4; i++) s += `<rect x="${i * 4.5}" y="${11 - i * 3}" width="3" height="${3 + i * 3}" rx=".8" fill="${i < n ? '#f2f2f2' : '#444'}"/>`;
  return s + '</svg>';
}
function topStat() {
  const el = $('#stat'); if (!el) return;
  el.textContent = '';
  if (S) {
    if (S.rec && S.rec.on) el.append(h('span', { title: 'Іде запис' }, h('i', { class: 'recdot' }), mmss(S.rec.sec)));
    if (S.bat && !S.dev.noBat && S.bat.pct >= 0) {
      const t = S.bat.chg ? 'заряджається' : S.bat.full ? 'заряджено' : '';
      el.append(h('span', { class: 'batt', title: `Батарея ${S.bat.pct}% · ${(S.bat.mv / 1000).toFixed(2)} В${t ? ' · ' + t : ''}` }, h('span', { html: batSvg(S.bat.pct, S.bat.chg) }), S.bat.pct + '%'));
    }
    if (S.rssi) el.append(h('span', { class: 'sig', title: `${S.ssid} · ${S.rssi} дБм`, html: sigSvg(S.rssi) }));
    if (S.time) el.append(h('span', { class: 'clock' }, S.time));
  }
  el.append(h('i', { class: 'conn' + (wsOk && stFails < 3 ? ' ok' : ''), title: wsOk ? 'Радіо на зв\'язку' : 'Немає зв\'язку з радіо' }));
}

/* ---------------------------------------------------------------- плеєр */
function isSerm() { return S && S.serm && S.serm.on; }
function srcInfo() {
  if (isSerm()) return { i: 'book', t: 'Проповідь' };
  if (W.mode === 1) return { i: 'sd', t: 'Картка пам\'яті' };
  return { i: 'radio', t: 'Радіо' };
}
function coverEl(big) {
  if (isSerm() && S.serm.c) {
    const site = S.sermSite || 'https://site.roman-home.keenetic.pro';
    const src = /^https?:/.test(S.serm.c) ? S.serm.c : site + S.serm.c;
    return h('div', { class: 'cover wide' }, h('img', { src, alt: '' }));
  }
  if (W.mode === 1) return h('div', { class: 'cover' + (big ? '' : ' logo') }, ic('sd'));
  const st = PL[W.cur - 1];
  if (!st) return h('div', { class: 'cover' }, ic('radio'));
  /*  той самий елемент, що й у списку: логотип довантажується в нього сам  */
  return logoEl(st.name, st.url, big ? 'cover' : '');
}
function mini() {
  return h('div', { class: 'mini', id: 'mini' });
}
let miniSig = '';
function miniUpdate() {
  const m = $('#mini'); if (!m) return;
  const serm = isSerm();
  const name = serm ? S.serm.t : (W.name || 'Нічого не грає');
  const sub = serm ? S.serm.p : W.title;
  const sig = [name, sub, W.playing, W.mode, W.cur, serm, LOGO.url.size, S && S.rec && S.rec.on].join('|');
  if (sig !== miniSig) {
    miniSig = sig;
    m.textContent = '';
    const c = coverEl(false); c.classList.add('logo');
    m.append(c,
      h('div', { class: 'tx', onclick: () => { location.hash = '#/player'; } }, h('b', null, S && S.rec && S.rec.on ? h('i', { class: 'recdot', title: 'Іде запис' }) : null, name), h('small', null, sub || '')),
      ibtn('prev', 'Попередня', () => serm ? setx({ sermRel: -1 }) : send('prev=1'), 'ghost'),
      ibtn(W.playing ? 'pause' : 'play', W.playing ? 'Пауза' : 'Грати', () => send('toggle=1'), 'acc'),
      ibtn('next', 'Наступна', () => serm ? setx({ sermRel: 1 }) : send('next=1'), 'ghost'),
      h('div', { class: 'v' }, ic('vol'), volRange('mv')));
  }
  const r = $('#mv'); if (r && !held(r)) { r.value = W.vol; fillRange(r); }
}
let volT = 0;
function volRange(id) {
  const r = range(0, 254, W.vol, v => { W.vol = v; clearTimeout(volT); volT = setTimeout(() => send('volume=' + v), 60); const o = $('#vo'); if (o) o.textContent = Math.round(v / 2.54) + '%'; });
  r.id = id; r.setAttribute('aria-label', 'Гучність');
  return r;
}

const VIEWS = {};
VIEWS.player = page => {
  const np = h('div', { class: 'np' });
  const ctl = h('div', { class: 'ctl' });
  const seek = h('div', { class: 'seek' });
  const vol = h('div', { class: 'vol' });
  const top = card(null, np, ctl, seek, vol);
  const src = h('div');
  const rec = h('div');
  const sleep = h('div');
  const favs = h('div', { class: 'favs' });
  page.append(top,
    card('Джерело', src),
    card('Обране', favs, h('p', { class: 'note' }, 'Торкніться, щоб увімкнути. Змінити кнопки — у розділі ', h('a', { href: '#/fav' }, 'Обране'), '.')),
    h('div', { class: 'grid2' }, card('Таймер сну', sleep), card('Запис ефіру', rec)));

  let sig = '', seekDrag = false;
  const vr = volRange('pv');
  vol.append(ic('vol0'), vr, h('output', { id: 'vo' }, Math.round(W.vol / 2.54) + '%'));
  const sleepSeg = seg([[0, 'Вимк'], [15, '15'], [30, '30'], [60, '60'], [90, '90']], -1, v => setx({ sleep: v }));
  const sleepNote = h('p', { class: 'note' });
  sleep.append(sleepSeg, sleepNote);

  live(what => {
    const serm = isSerm();
    const s2 = [W.name, W.title, W.playing, W.mode, W.cur, W.bitrate, W.fmt, serm, serm && S.serm.t, LOGO.url.size, PL.length].join('|');
    if (s2 !== sig) {
      sig = s2;
      const si = srcInfo();
      np.textContent = '';
      np.append(coverEl(true), h('div', null,
        h('div', { class: 'src' }, ic(si.i), si.t, W.playing ? '' : ' · пауза'),
        h('h3', null, serm ? S.serm.t : (W.name || 'Нічого не грає')),
        h('div', { class: 'ttl' }, serm ? S.serm.p : (W.title && W.title !== W.name ? W.title : '')),
        h('div', { class: 'meta' }, W.bitrate && W.playing ? `${W.bitrate} кбіт/с${W.fmt && W.fmt !== 'bitrate' ? ' · ' + W.fmt : ''}` : '')));
      ctl.textContent = '';
      ctl.append(
        ibtn('prev', serm ? 'Попередня проповідь' : 'Попередня', () => serm ? setx({ sermRel: -1 }) : send('prev=1')),
        ibtn(W.playing ? 'pause' : 'play', W.playing ? 'Пауза' : 'Грати', () => send('toggle=1'), 'acc'),
        ibtn('next', serm ? 'Наступна проповідь' : 'Наступна', () => serm ? setx({ sermRel: 1 }) : send('next=1')));
    }
    /*  перемотка: проповідь — секунди з сайту; картка — байти файлу  */
    if (!seekDrag) {
      seek.textContent = '';
      if (serm && S.serm.dur) {
        const r = range(0, S.serm.dur, S.serm.pos, v => { seekDrag = true; t1.textContent = mmss(v); }, v => { seekDrag = false; setx({ seek: Math.max(1, v) }); });
        const t1 = h('span', null, mmss(S.serm.pos));
        seek.append(r, h('div', { class: 't' }, t1, h('span', null, mmss(S.serm.dur))));
      } else if (W.mode === 1 && W.sdmax > W.sdmin) {
        const r = range(W.sdmin, W.sdmax, W.sdpos || W.sdmin, () => { seekDrag = true; }, v => { seekDrag = false; send('sdpos=' + v); });
        seek.append(r, h('div', { class: 't' }, h('span', null, mmss(W.sdtpos)), h('span', null, mmss(W.sdtend))));
      }
    }
    if (!held(vr)) { vr.value = W.vol; fillRange(vr); $('#vo').textContent = Math.round(W.vol / 2.54) + '%'; }
    if (!S) return;
    /*  джерело  */
    const ssig = [W.mode, S.dev.noSd, W.snuffle, serm].join('|');
    if (src.dataset.sig !== ssig) {
      src.dataset.sig = ssig; src.textContent = '';
      const items = [[0, 'Радіо']]; if (!S.dev.noSd) items.push([1, 'Картка пам\'яті']);
      src.append(h('div', { class: 'bar' }, seg(items, W.mode, v => { send('newmode=' + v); }),
        h('span', { class: 'sp' }),
        W.mode === 1 ? btn(W.snuffle ? 'Перемішано' : 'Підряд', 'shuffle', () => send('snuffle=' + (W.snuffle ? 'false' : 'true')), W.snuffle ? 'acc sm' : 'sm') : null,
        h('a', { class: 'btn sm', href: '#/sermons' }, ic('book'), 'Проповіді')));
      if (S.dev.noSd) src.append(h('p', { class: 'note' }, 'Функції картки вимкнено в розділі «Розробник».'));
    }
    /*  таймер сну  */
    sleepSeg.set(S.sleep.set);
    sleepNote.textContent = S.sleep.set ? `Вимкнеться через ${mmss(S.sleep.left)}. Останні 20 секунд звук плавно стихає.` : 'Радіо гратиме, доки його не вимкнуть.';
    /*  запис  */
    const rsig = [S.rec.on, S.rec.sec, S.rec.err, W.mode, serm, S.dev.noSd].join('|');
    if (rec.dataset.sig !== rsig) {
      rec.dataset.sig = rsig; rec.textContent = '';
      if (S.rec.on) {
        rec.append(h('div', { class: 'bar' }, h('span', null, h('i', { class: 'recdot' }), `Записую ${mmss(S.rec.sec)} · ${size(S.rec.bytes)}`), h('span', { class: 'sp' }), btn('Зупинити', 'stop', () => setx({ rec: 0 }), 'sm')));
        rec.append(h('p', { class: 'note' }, S.rec.file));
      } else {
        const can = W.mode === 0 && !serm && W.playing && !S.dev.noSd;
        rec.append(h('div', { class: 'bar' }, btn('Записати ефір', 'rec', () => setx({ rec: 1 }), can ? 'sm' : 'sm'), h('span', { class: 'sp' }), h('a', { href: '#/records', class: 'btn sm ghost' }, 'Записи')));
        rec.append(h('p', { class: 'note' }, S.dev.noSd ? 'Функції картки вимкнено.' : !W.playing ? 'Спершу увімкніть радіостанцію.' : W.mode ? 'Записати можна лише радіо.' : 'Потік пишеться на картку як є, без втрати якості.'));
      }
    }
    /*  обране  */
    const fsig = JSON.stringify(S.fav) + S.favOn;
    if (favs.dataset.sig !== fsig) {
      favs.dataset.sig = fsig; favs.textContent = '';
      S.fav.forEach((f, i) => favs.append(f.n
        ? h('button', { type: 'button', class: 'fav' + (S.favOn === i ? ' on' : ''), onclick: () => setx({ favPlay: i }) }, h('span', { class: 'n' }, f.n), h('span', { class: 'k' }, S.favOn === i ? 'грає' : String(i + 1)))
        : h('div', { class: 'fav empty' }, h('span', { class: 'n' }, 'порожньо'), h('span', { class: 'k' }, String(i + 1)))));
    }
  });
};

/* ---------------------------------------------------------------- станції */
/*  Вкладки розділу «Станції»: радіостанції (редактор) і треки картки.  */
let stTab = null;
function stTabs(page, cur) {
  if (S && S.dev.noSd) return null;
  return h('div', { style: { marginBottom: '14px' } }, seg([['radio', 'Радіостанції'], ['sd', 'Картка пам\'яті']], cur, v => {
    stTab = v; page.textContent = ''; (v === 'sd' ? VIEWS.sdlist : VIEWS.stations)(page);
  }));
}

/*  Треки картки — як список станцій: пошук, поточний підсвічено, дотик — грати.  */
VIEWS.sdlist = page => {
  if (pageLive) { listeners.delete(pageLive); pageLive = null; }
  const tabs = stTabs(page, 'sd');
  const q = h('input', { type: 'search', placeholder: 'Пошук за назвою чи текою', 'aria-label': 'Пошук' });
  const info = h('span', { class: 'note', style: { margin: 0 } });
  const bar = h('div', { class: 'bar' });
  const list = h('div', { class: 'list' });
  page.append(tabs, h('div', { class: 'tools' }, h('div', { class: 'bar' }, h('div', { class: 'search' }, ic('search'), q), bar), h('div', { style: { marginTop: '8px' } }, info)), list);
  let limit = 100, lastSig = '';
  const dir = p => { const k = p.lastIndexOf('/'); return k > 0 ? p.slice(0, k) : ''; };
  function draw() {
    bar.textContent = '';
    list.textContent = '';
    if (W.mode !== 1) {
      info.textContent = '';
      list.append(h('div', { class: 'none' }, h('p', { style: { marginTop: 0 } }, 'Зараз радіо грає не з картки. Перейдіть на картку — і тут з\'явиться список треків.'),
        btn('Перейти на картку', 'sd', () => { send('newmode=1'); list.textContent = ''; list.append(h('div', { class: 'none' }, 'Перемикаю й читаю картку…')); }, 'acc')));
      return;
    }
    bar.append(btn(W.snuffle ? 'Перемішано' : 'Підряд', 'shuffle', () => send('snuffle=' + (W.snuffle ? 'false' : 'true')), W.snuffle ? 'acc sm' : 'sm'),
      btn('Радіо', 'radio', () => send('newmode=0'), 'sm ghost'));
    const f = q.value.trim().toLowerCase();
    const shown = [];
    PL.forEach((t, i) => { if (!f || t.name.toLowerCase().includes(f) || t.url.toLowerCase().includes(f)) shown.push(i); });
    info.textContent = shown.length === PL.length ? `${PL.length} ${plural(PL.length, 'трек', 'треки', 'треків')} на картці` : `Знайдено ${shown.length} з ${PL.length}`;
    /*  поточний трек — завжди видно: з нього й починаємо показ, якщо він далеко  */
    let start = 0;
    const curPos = shown.indexOf(W.cur - 1);
    if (!f && curPos > limit - 10) start = Math.max(0, curPos - 20);
    shown.slice(start, start + limit).forEach(i => {
      const t = PL[i], on = i === W.cur - 1;
      list.append(h('div', { class: 'st' + (on ? ' on' : ''), style: { minHeight: '48px' } },
        h('span', { class: 'no', style: { display: 'block' } }, i + 1),
        h('div', { class: 'logo', style: { background: 'var(--pan2)', color: on ? 'var(--acc)' : 'var(--dim)' } }, ic(on && W.playing ? 'play' : 'file')),
        h('div', { class: 'tx', onclick: () => send('play=' + (i + 1)), title: 'Грати на радіо' },
          h('div', { class: 'nm' }, t.name), h('div', { class: 'ur' }, dir(t.url) || '/'))));
    });
    if (start > 0) list.prepend(h('div', { class: 'none' }, btn('Показати з початку', 'up', () => { limit = start + limit; draw(); }, 'sm')));
    if (shown.length > start + limit) list.append(h('div', { class: 'none' }, btn(`Показати ще (${shown.length - start - limit})`, 'down', () => { limit += 200; draw(); }, 'sm')));
    if (!shown.length) list.append(h('div', { class: 'none' }, PL.length ? 'Нічого не знайдено.' : 'На картці немає треків (або вона ще читається).'));
  }
  let qt = 0;
  q.addEventListener('input', () => { clearTimeout(qt); qt = setTimeout(() => { limit = 100; draw(); }, 150); });
  live(what => { const s = [W.mode, W.cur, W.playing, W.snuffle, PL.length].join('|'); if (s !== lastSig || what === 'pl') { lastSig = s; draw(); } });
};

VIEWS.stations = async page => {
  if (stTab === null) stTab = W.mode === 1 ? 'sd' : 'radio';
  if (stTab === 'sd' && !(S && S.dev.noSd)) { VIEWS.sdlist(page); return; }
  const tabs = stTabs(page, 'radio');
  if (tabs) page.append(tabs);
  const q = h('input', { type: 'search', placeholder: 'Пошук за назвою чи адресою', 'aria-label': 'Пошук' });
  const tools = h('div', { class: 'tools' }, h('div', { class: 'bar' },
    h('div', { class: 'search' }, ic('search'), q),
    btn('Додати', 'plus', () => editStation(-1), 'acc'),
    btn('Знайти в каталогах', 'globe', () => { location.hash = '#/find'; }),
    btn('Імпорт', 'upload', importFile),
    btn('Експорт', 'download', exportMenu)));
  const dirty = h('div', { class: 'dirty', hidden: true });
  const sdNote = h('div');
  const list = h('div', { class: 'list' }, h('div', { class: 'none' }, 'Завантажую список…'));
  page.append(h('p', { class: 'lead' }, 'Натисніть на станцію, щоб увімкнути її на радіо. Зміни в списку набудуть сили після «Зберегти».'), tools, sdNote, dirty, list);
  try { await loadEditor(); } catch (e) { list.textContent = ''; list.append(h('div', { class: 'none' }, 'Не вдалося прочитати список: ' + e.message)); return; }

  let dragFrom = -1;
  function draw() {
    const d = edDirty();
    dirty.hidden = !d;
    dirty.textContent = '';
    if (d) dirty.append(h('b', null, 'Список змінено, але ще не збережено на радіо.'), btn('Скасувати зміни', null, async () => { if (await confirmBox('Скасувати зміни?', 'Список повернеться до того, що зараз на радіо.', 'Скасувати зміни')) { ED.list = parseCSVText(ED.orig); draw(); } }, 'ghost sm'), btn('Зберегти', 'check', saveStations, 'acc sm'));
    sdNote.textContent = '';
    if (W.mode === 1) sdNote.append(h('div', { class: 'dirty' }, h('b', null, 'Зараз грає картка пам\'яті. Список станцій можна правити, а зберегти — коли грає радіо.'), btn('Перейти на радіо', 'radio', () => send('newmode=0'), 'sm')));
    const f = q.value.trim().toLowerCase();
    list.textContent = '';
    const playingUrl = S && S.url;
    let shown = 0;
    ED.list.forEach((s, i) => {
      if (f && !(s.name.toLowerCase().includes(f) || s.url.toLowerCase().includes(f))) return;
      shown++;
      const on = W.mode === 0 && playingUrl && s.url === playingUrl && !isSerm();
      const favI = S ? S.fav.findIndex(x => x.u === s.url) : -1;
      const r = h('div', { class: 'st' + (on ? ' on' : ''), 'data-i': i },
        f ? null : h('span', { class: 'hd', title: 'Перетягніть, щоб змінити порядок' }, ic('grip')),
        h('span', { class: 'no' }, i + 1),
        logoEl(s.name, s.url),
        h('div', { class: 'tx', onclick: () => playStation(i), title: 'Увімкнути на радіо' },
          h('div', { class: 'nm' }, s.name, s.ovol ? h('span', { class: 'badge' }, (s.ovol > 0 ? '+' : '') + s.ovol) : null),
          h('div', { class: 'ur' }, s.url)),
        h('div', { class: 'act' },
          ibtn(favI >= 0 ? 'starf' : 'star', favI >= 0 ? `В обраному (${favI + 1})` : 'Додати в обране', () => pickFavSlot(s), 'ghost sm'),
          ibtn('edit', 'Змінити', () => editStation(i), 'ghost sm')));
      list.append(r);
    });
    if (!shown) list.append(h('div', { class: 'none' }, ED.list.length ? 'Нічого не знайдено.' : 'Список порожній. Додайте станцію вручну, з каталогу або з файлу.'));
  }
  /*  перетягування за ручку — і мишею, і пальцем  */
  list.addEventListener('pointerdown', e => {
    const hd = e.target.closest('.hd'); if (!hd) return;
    const r = hd.closest('.st'); dragFrom = +r.dataset.i; r.classList.add('drag');
    hd.setPointerCapture(e.pointerId); e.preventDefault();
    let over = -1;
    const move = ev => {
      const el = document.elementFromPoint(ev.clientX, ev.clientY), t = el && el.closest('.st');
      list.querySelectorAll('.over').forEach(x => x.classList.remove('over'));
      if (t) { over = +t.dataset.i; t.classList.add('over'); }
      if (ev.clientY < 90) window.scrollBy(0, -12); else if (ev.clientY > innerHeight - 90) window.scrollBy(0, 12);
    };
    const up = () => {
      hd.removeEventListener('pointermove', move); hd.removeEventListener('pointerup', up); hd.removeEventListener('pointercancel', up);
      if (over >= 0 && over !== dragFrom) { const [it] = ED.list.splice(dragFrom, 1); ED.list.splice(over, 0, it); }
      dragFrom = -1; draw();
    };
    hd.addEventListener('pointermove', move); hd.addEventListener('pointerup', up); hd.addEventListener('pointercancel', up);
  });
  q.addEventListener('input', draw);
  VIEWS.stations.redraw = draw;
  let lastSig = '';
  live(what => { const s = [W.mode, S && S.url, S && JSON.stringify(S.fav), LOGO.url.size, isSerm()].join('|'); if (what === 'logos' || s !== lastSig) { lastSig = s; draw(); } });
};
function redrawStations() { if (curPage === 'stations' && VIEWS.stations.redraw) VIEWS.stations.redraw(); }

async function playStation(i) {
  const s = ED.list[i];
  if (W.mode === 1) { toast('Спершу перейдіть на радіо', true); return; }
  /*  плата грає за номером у збереженому списку — шукаємо станцію там  */
  const k = PL.findIndex(x => x.url === s.url);
  if (k < 0) { toast('Станції ще немає на радіо — збережіть список', true); return; }
  send('play=' + (k + 1));
}

function editStation(i) {
  const isNew = i < 0;
  const s = isNew ? { name: '', url: '', ovol: 0 } : ED.list[i];
  const nm = h('input', { type: 'text', value: s.name, maxlength: 120, placeholder: 'Наприклад: Радіо Промінь' });
  const ur = h('input', { type: 'url', value: s.url, placeholder: 'http://…', spellcheck: 'false' });
  const ovOut = h('output', null, (s.ovol > 0 ? '+' : '') + s.ovol);
  const ov = range(-30, 30, s.ovol, v => { ovOut.textContent = (v > 0 ? '+' : '') + v; });
  const pos = h('input', { type: 'number', min: 1, max: ED.list.length + (isNew ? 1 : 0), value: isNew ? ED.list.length + 1 : i + 1 });
  const err = h('p', { class: 'note', style: { color: 'var(--bad)' } });
  const audio = h('audio', { preload: 'none' });
  let testing = false;
  const testBtn = btn('Послухати тут', 'headph', () => {
    if (testing) { audio.pause(); audio.removeAttribute('src'); audio.load(); testing = false; testBtn.lastChild.textContent = 'Послухати тут'; return; }
    if (!/^https?:\/\//i.test(ur.value.trim())) { err.textContent = 'Адреса має починатися з http:// або https://'; return; }
    audio.src = ur.value.trim(); audio.play().then(() => { err.textContent = ''; }).catch(() => { err.textContent = 'Браузер не зміг відкрити потік. Радіо може впоратися — перевірте на ньому.'; });
    testing = true; testBtn.lastChild.textContent = 'Зупинити';
  }, 'sm');
  const logoBox = h('div', { class: 'bar' });
  const drawLogo = () => {
    logoBox.textContent = '';
    if (!ur.value.trim()) { logoBox.append(h('span', { class: 'note', style: { margin: 0 } }, 'Логотип можна задати після того, як буде адреса.')); return; }
    const file = h('input', { type: 'file', accept: 'image/*', hidden: true });
    file.onchange = async () => { if (!file.files[0]) return; try { await uploadLogo(ur.value.trim(), file.files[0]); toast('Логотип збережено'); drawLogo(); redrawStations(); } catch (e) { toast('Логотип не збережено: ' + e.message, true); } };
    const crc = crc32(ur.value.trim()), has = LOGO.have.has(crc + '.565') || LOGO.have.has(crc + '.jpg');
    logoBox.append(logoEl(nm.value, ur.value.trim()), file,
      btn('Свій логотип', 'image', () => file.click(), 'sm'),
      has ? btn('Знайти заново', 'refresh', async () => { await setx({ logoDel: crc }); setTimeout(async () => { await loadLogos(); drawLogo(); redrawStations(); }, 600); }, 'sm ghost') : h('span', { class: 'note', style: { margin: 0 } }, 'Радіо саме шукає логотип у каталозі, коли станція заграє.'));
  };
  ur.addEventListener('change', drawLogo);
  drawLogo();
  const body = h('div', null,
    h('label', { class: 'fld' }, h('span', null, 'Назва'), nm),
    h('label', { class: 'fld' }, h('span', null, 'Адреса потоку'), ur),
    h('div', { class: 'bar', style: { marginBottom: '12px' } }, testBtn, audio),
    h('div', { class: 'fld' }, h('span', null, 'Корекція гучності станції'), h('div', { class: 'rng' }, ov, ovOut)),
    h('label', { class: 'fld inl' }, h('span', null, 'Місце у списку'), h('div', { style: { width: '110px' } }, pos)),
    h('div', { class: 'fld' }, h('span', null, 'Логотип'), logoBox),
    err);
  const stopAudio = () => { audio.pause(); audio.removeAttribute('src'); };
  const d = dialog(isNew ? 'Нова станція' : 'Станція', body, [
    isNew ? null : btn('Видалити', 'trash', async () => {
      if (!await confirmBox('Видалити станцію?', `«${s.name}» зникне зі списку після збереження.`, 'Видалити', true)) return;
      ED.list.splice(i, 1); stopAudio(); d.close(); redrawStations();
    }, 'ghost bad'),
    '|',
    btn('Скасувати', null, () => { stopAudio(); d.close(); }, 'ghost'),
    btn(isNew ? 'Додати' : 'Готово', 'check', () => {
      const c = cleanSt({ name: nm.value, url: ur.value, ovol: ov.value });
      if (!c.name) { err.textContent = 'Вкажіть назву.'; nm.focus(); return; }
      if (!/^https?:\/\/[^\s]+$/i.test(c.url)) { err.textContent = 'Адреса має починатися з http:// або https://'; ur.focus(); return; }
      if (bytes(c.url) > 160) { err.textContent = 'Задовга адреса: радіо вміщує до 160 знаків.'; return; }
      const dup = ED.list.findIndex((x, k) => x.url === c.url && k !== i);
      if (dup >= 0) { err.textContent = `Така адреса вже є: «${ED.list[dup].name}» (№${dup + 1}).`; return; }
      let p = Math.max(1, Math.min(ED.list.length + (isNew ? 1 : 0), parseInt(pos.value) || 1)) - 1;
      if (isNew) ED.list.splice(p, 0, c);
      else { ED.list.splice(i, 1); ED.list.splice(p, 0, c); }
      stopAudio(); d.close(); redrawStations();
    }, 'acc'),
  ], { onclose: stopAudio });
}

async function saveStations() {
  if (W.mode === 1) { toast('Зберегти список можна лише в режимі радіо', true); return; }
  if (!ED.list.length && !await confirmBox('Порожній список', 'На радіо не лишиться жодної станції. Зберегти так?', 'Зберегти', true)) return;
  const wasUrl = S && S.url, wasPlaying = W.playing && !isSerm();
  const text = serialize(ED.list.map(cleanSt));
  try {
    await api('/api/stations', { method: 'POST', body: new Blob([text], { type: 'application/octet-stream' }) });
  } catch (e) { toast('Не збережено: ' + e.message, true); return; }
  ED.orig = text; ED.list = parseCSVText(text);
  toast('Список збережено на радіо');
  setTimeout(async () => {
    await loadPlayList();
    /*  станцію, що грала, у новому списку могли пересунути — нехай плата знає її новий номер  */
    if (wasPlaying && wasUrl) { const k = PL.findIndex(x => x.url === wasUrl); if (k >= 0 && k + 1 !== W.cur) send('play=' + (k + 1)); }
    redrawStations();
  }, 900);
}

function exportMenu() {
  const list = edDirty() ? ED.list : parseCSVText(ED.orig);
  const d = dialog('Експорт списку', h('div', null,
    h('p', { class: 'note', style: { marginTop: 0 } }, `${list.length} ${plural(list.length, 'станція', 'станції', 'станцій')}${edDirty() ? ', разом із незбереженими змінами' : ''}.`),
    h('div', { class: 'bar', style: { marginTop: '12px' } },
      btn('CSV для радіо', 'download', () => { download('playlist.csv', serialize(list), 'text/csv'); d.close(); }),
      btn('M3U для програвачів', 'download', () => { download('potuzhne-radio.m3u', '#EXTM3U\n' + list.map(s => `#EXTINF:-1,${s.name}\n${s.url}`).join('\n') + '\n', 'audio/x-mpegurl'); d.close(); }))),
    [btn('Закрити', null, () => d.close(), 'ghost')]);
}

function importFile() {
  const f = h('input', { type: 'file', accept: '.csv,.m3u,.m3u8,.pls,.txt,.json,text/*' });
  f.onchange = async () => {
    const file = f.files[0]; if (!file) return;
    const text = await file.text();
    const found = parseAny(text, file.name).map(cleanSt).filter(s => s.name && s.url);
    if (!found.length) { toast('У файлі не знайшлося жодної станції', true); return; }
    importReview(found, file.name);
  };
  f.click();
}
function importReview(found, from) {
  const have = new Set(ED.list.map(s => s.url));
  const boxes = found.map(s => { const dup = have.has(s.url); return { s, dup, cb: h('input', { type: 'checkbox', checked: !dup }) }; });
  let mode = 'add';
  const cnt = h('span', { class: 'note', style: { margin: 0 } });
  const upd = () => { const n = boxes.filter(b => b.cb.checked).length; cnt.textContent = `Вибрано ${n} з ${boxes.length}`; };
  boxes.forEach(b => b.cb.addEventListener('change', upd));
  const body = h('div', null,
    h('p', { class: 'note', style: { marginTop: 0 } }, `У «${from}» знайдено ${found.length} ${plural(found.length, 'станцію', 'станції', 'станцій')}. Ті, що вже є у списку, не позначено.`),
    seg([['add', 'Додати в кінець'], ['replace', 'Замінити весь список']], 'add', v => { mode = v; }),
    h('div', { class: 'imp' }, boxes.map(b => h('label', null, b.cb, h('div', null, b.s.name, b.dup ? h('span', { class: 'badge' }, 'вже є') : null, h('small', null, b.s.url))))),
    h('div', { class: 'bar' }, btn('Усі', null, () => { boxes.forEach(b => b.cb.checked = true); upd(); }, 'sm ghost'), btn('Жодної', null, () => { boxes.forEach(b => b.cb.checked = false); upd(); }, 'sm ghost'), h('span', { class: 'sp' }), cnt));
  upd();
  const d = dialog('Імпорт станцій', body, [
    btn('Скасувати', null, () => d.close(), 'ghost'),
    btn('Перенести в список', 'check', () => {
      const pick = boxes.filter(b => b.cb.checked).map(b => b.s);
      if (mode === 'replace') ED.list = pick; else ED.list.push(...pick.filter(s => !have.has(s.url)));
      d.close(); redrawStations();
      toast(`Перенесено ${pick.length}. Перегляньте список і натисніть «Зберегти».`);
    }, 'acc')]);
}

/* ---------------------------------------------------------------- пошук станцій
   Вільні каталоги, шукає сама сторінка (радіо в пошуку не бере участі):
   radio-browser.info — понад 50 тисяч станцій, дані суспільного надбання:
     назва, країна, мова, жанр, формат, якість, популярність;
   Icecast (dir.xiph.org) — сервери Icecast з усього світу: назва, жанр, формат.
   Знайдене додається в список станцій; на радіо — кнопкою «Зберегти на радіо».  */
const RB_HOSTS = ['de1.api.radio-browser.info', 'all.api.radio-browser.info', 'de2.api.radio-browser.info', 'fi1.api.radio-browser.info'];
const FIND = { host: '', countries: null, languages: null, tags: null, xiph: null, xiphP: null };
/*  Формати, які декодує радіо (бібліотека звуку: MP3, AAC/AAC+, FLAC, OGG-FLAC).  */
const radioPlays = codec => !codec || /^(mp3|mpeg|aac\+?|aacp|he-aac|flac|unknown)$/i.test(String(codec).trim());

async function rbGet(path, params) {
  const qs = params ? '?' + new URLSearchParams(params) : '';
  const hosts = FIND.host ? [FIND.host, ...RB_HOSTS.filter(x => x !== FIND.host)] : RB_HOSTS;
  for (const host of hosts) {
    const ac = new AbortController(), t = setTimeout(() => ac.abort(), 9000);
    try {
      const r = await fetch(`https://${host}/json/${path}${qs}`, { signal: ac.signal });
      clearTimeout(t);
      if (r.ok) { FIND.host = host; return await r.json(); }
    } catch (e) { clearTimeout(t); }
  }
  throw new Error('каталог radio-browser.info не відповідає — перевірте інтернет на цьому пристрої');
}

const ukRegion = (() => { try { return new Intl.DisplayNames(['uk'], { type: 'region' }); } catch (e) { return null; } })();
const ukLang = (() => { try { return new Intl.DisplayNames(['uk'], { type: 'language' }); } catch (e) { return null; } })();
const capital = s => s ? s[0].toUpperCase() + s.slice(1) : s;
function countryName(code, fallback) { try { return (code && ukRegion && ukRegion.of(code.toUpperCase())) || fallback || code; } catch (e) { return fallback || code; } }
function langName(iso, name) {
  try { if (iso && ukLang) { const n = ukLang.of(iso.split(',')[0].trim()); if (n && n.toLowerCase() !== iso.toLowerCase()) return capital(n); } } catch (e) {}
  return capital(name || iso || '');
}

async function findLists() {
  if (!FIND.countries) {
    const [c, l, t] = await Promise.all([
      rbGet('countries', { hidebroken: 'true' }),
      rbGet('languages', { hidebroken: 'true', order: 'stationcount', reverse: 'true', limit: 400 }),
      rbGet('tags', { hidebroken: 'true', order: 'stationcount', reverse: 'true', limit: 400 })]);
    FIND.countries = c.filter(x => x.iso_3166_1 && x.stationcount > 0)
      .map(x => ({ code: x.iso_3166_1.toUpperCase(), name: countryName(x.iso_3166_1, x.name), n: x.stationcount }))
      .sort((a, b) => a.name.localeCompare(b.name, 'uk'));
    FIND.languages = l.filter(x => x.name && x.stationcount > 1)
      .map(x => ({ key: x.name, name: langName(x.iso_639, x.name), n: x.stationcount }))
      .sort((a, b) => a.name.localeCompare(b.name, 'uk'));
    FIND.tags = t.filter(x => x.name && x.stationcount > 4).map(x => x.name);
    FIND.langMap = new Map(FIND.languages.map(x => [x.key, x.name]));
  }
}

/*  Каталог Icecast — один XML на ~9 МБ: вантажимо раз, далі шукаємо в пам'яті.  */
function xiphLoad() {
  if (FIND.xiph) return Promise.resolve(FIND.xiph);
  if (!FIND.xiphP) FIND.xiphP = (async () => {
    const r = await fetch('https://dir.xiph.org/yp.xml');
    if (!r.ok) throw new Error('каталог Icecast не відповідає');
    const doc = new DOMParser().parseFromString(await r.text(), 'text/xml');
    const out = [], seen = new Set();
    const typeCodec = t => /mpeg/.test(t) ? 'MP3' : /aacp/.test(t) ? 'AAC+' : /aac/.test(t) ? 'AAC' : /flac/.test(t) ? 'FLAC' : /ogg/.test(t) ? 'OGG' : (t || '').replace(/^.*\//, '').toUpperCase();
    for (const e of doc.getElementsByTagName('entry')) {
      const g = n => (e.getElementsByTagName(n)[0] || {}).textContent || '';
      const url = g('listen_url').trim();
      if (!/^https?:\/\//.test(url) || seen.has(url)) continue;
      seen.add(url);
      out.push({ name: g('server_name').trim() || url, url, codec: typeCodec(g('server_type')), bitrate: parseInt(g('bitrate')) || 0, tags: g('genre').trim(), song: g('current_song').trim() });
    }
    FIND.xiph = out;
    return out;
  })().catch(e => { FIND.xiphP = null; throw e; });
  return FIND.xiphP;
}

VIEWS.find = async page => {
  let F = { src: 'rb', name: '', country: 'UA', lang: '', tag: '', codec: '', br: 0, order: 'votes', plays: true, working: true };
  try { Object.assign(F, JSON.parse(localStorage.getItem('potuzhne.find') || '{}')); } catch (e) {}
  const keep = () => { try { localStorage.setItem('potuzhne.find', JSON.stringify(F)); } catch (e) {} };

  const q = h('input', { type: 'search', placeholder: 'Назва станції — «Промінь», «jazz», «Hit FM»', 'aria-label': 'Назва станції', value: F.name });
  const country = h('select', { 'aria-label': 'Країна' }, h('option', { value: '' }, 'Усі країни'));
  const lang = h('select', { 'aria-label': 'Мова' }, h('option', { value: '' }, 'Усі мови'));
  const tagList = h('datalist', { id: 'findtags' });
  const tag = h('input', { type: 'search', placeholder: 'наприклад: news, rock, christian', list: 'findtags', 'aria-label': 'Жанр', value: F.tag });
  const codec = h('select', { 'aria-label': 'Формат' }, [['', 'Будь-який'], ['MP3', 'MP3'], ['AAC', 'AAC'], ['AAC+', 'AAC+'], ['FLAC', 'FLAC']].map(([v, t]) => h('option', { value: v }, t)));
  const br = h('select', { 'aria-label': 'Якість' }, [[0, 'Будь-яка'], [64, 'від 64 кбіт/с'], [128, 'від 128 кбіт/с'], [192, 'від 192 кбіт/с'], [320, '320 кбіт/с']].map(([v, t]) => h('option', { value: v }, t)));
  const order = h('select', { 'aria-label': 'Порядок' });
  const orders = {
    rb: [['votes', 'Найпопулярніші'], ['clickcount', 'Найчастіше слухають'], ['name', 'За назвою'], ['bitrate', 'Найкраща якість'], ['lastchangetime', 'Нові й оновлені']],
    xiph: [['name', 'За назвою'], ['bitrate', 'Найкраща якість']] };
  codec.value = F.codec; br.value = F.br;
  const src = seg([['rb', 'radio-browser.info'], ['xiph', 'Icecast (dir.xiph.org)']], F.src, v => { F.src = v; keep(); srcUi(); run(); });
  const playsSw = sw(F.plays, v => { F.plays = v; keep(); run(); });
  const workSw = sw(F.working, v => { F.working = v; keep(); run(); });
  const srcNote = h('p', { class: 'note', style: { margin: '8px 0 0' } });
  const dirty = h('div', { class: 'dirty', hidden: true });
  const info = h('span', { class: 'note', style: { margin: 0 } });
  const list = h('div', { class: 'list' }, h('div', { class: 'none' }, 'Готую каталог…'));
  const more = h('div');
  const audio = h('audio', { preload: 'none' });
  let playingBtn = null, offset = 0, items = [], token = 0;

  const workLbl = h('label', { class: 'bar', style: { gap: '8px' } }, workSw, h('span', null, 'Лише робочі (перевірені каталогом)'));
  const fld = (label, el) => h('label', { class: 'ffld' }, h('small', null, label), el);
  const cc = fld('Країна', country), ll = fld('Мова', lang);
  page.append(
    h('p', { class: 'lead' }, 'Пошук радіостанцій у вільних каталогах — за назвою, країною, мовою, жанром і якістю. Послухайте тут, додайте в список і збережіть на радіо.'),
    h('div', { class: 'tools', style: { position: 'static' } },
      h('div', { class: 'bar' }, src),
      h('div', { class: 'bar', style: { marginTop: '10px' } }, h('div', { class: 'search' }, ic('search'), q), btn('Шукати', 'search', () => run(), 'acc')),
      h('div', { class: 'ffilters' }, cc, ll, fld('Жанр', tag), fld('Формат', codec), fld('Якість', br), fld('Порядок', order)),
      h('div', { class: 'bar', style: { marginTop: '8px', gap: '16px' } },
        h('label', { class: 'bar', style: { gap: '8px' } }, playsSw, h('span', null, 'Лише ті, що зіграє радіо')),
        workLbl,
        h('span', { class: 'sp' }), info),
      srcNote, tagList),
    dirty, list, more, audio,
    h('p', { class: 'note' }, 'Каталоги ведуть слухачі й власники станцій; адреса може змінитись чи перестати працювати. «Послухати» грає на цьому пристрої, не на радіо. Радіо грає MP3, AAC і FLAC; OGG/Opus і WMA — ні.'));

  function srcUi() {
    const rb = F.src === 'rb';
    cc.hidden = !rb; ll.hidden = !rb; workLbl.hidden = !rb;
    order.textContent = '';
    for (const [v, t] of orders[F.src]) order.append(h('option', { value: v }, t));
    if (!orders[F.src].some(o => o[0] === F.order)) F.order = orders[F.src][0][0];
    order.value = F.order;
    srcNote.textContent = rb
      ? 'radio-browser.info — відкритий каталог, понад 50 000 станцій з усього світу; дані — суспільне надбання.'
      : 'Icecast (dir.xiph.org) — сервери Icecast, що самі записались у каталог: назва, жанр, формат. Країни й мови там немає. Перший пошук вантажить каталог (близько 9 МБ).';
  }

  function drawDirty() {
    const d = edDirty();
    dirty.hidden = !d;
    dirty.textContent = '';
    if (d) {
      const n = ED.list.length - parseCSVText(ED.orig).length;
      dirty.append(h('b', null, n > 0 ? `Додано ${n} ${plural(n, 'станцію', 'станції', 'станцій')} — на радіо їх ще немає.` : 'Список змінено, але ще не збережено на радіо.'),
        btn('До списку', 'radio', () => { location.hash = '#/stations'; }, 'ghost sm'),
        btn('Зберегти на радіо', 'check', async () => { await saveStations(); setTimeout(() => { drawDirty(); redrawRows(); }, 1000); }, 'acc sm'));
    }
  }

  const have = () => new Set(ED.list.map(s => s.url));
  let rowsRedraw = [];
  function redrawRows() { const hv = have(); rowsRedraw.forEach(f => f(hv)); }

  function rowEl(it) {
    const url = it.url;
    const plays = radioPlays(it.codec);
    const add = btn('Додати', 'plus', () => {
      if (have().has(url)) return;
      ED.list.push(cleanSt({ name: it.name, url, ovol: 0 }));
      toast(`«${it.name}» — у списку. Натисніть «Зберегти на радіо».`);
      drawDirty(); redrawRows();
    }, 'sm acc');
    const setAdded = hv => { const on = hv.has(url); add.disabled = on; add.className = 'btn sm ' + (on ? 'ghost' : 'acc'); add.replaceChildren(ic(on ? 'check' : 'plus'), on ? 'У списку' : 'Додати'); };
    rowsRedraw.push(setAdded);
    const pl = ibtn('play', 'Послухати тут', () => {
      if (playingBtn === pl) { audio.pause(); pl.replaceChildren(ic('play')); playingBtn = null; return; }
      if (playingBtn) playingBtn.replaceChildren(ic('play'));
      audio.src = url; audio.play().catch(() => { pl.replaceChildren(ic('play')); playingBtn = null; toast('Цей потік тут не відкрився — на радіо він усе одно може грати', true); });
      pl.replaceChildren(ic('pause')); playingBtn = pl;
      if (it.uuid) rbGet('url/' + it.uuid).catch(() => {});        /* каталог рахує прослуховування */
    }, 'sm ghost');
    const fav = h('div', { class: 'logo', style: { background: '#fff' } });
    if (it.favicon && /^https?:/.test(it.favicon)) {
      const im = h('img', { src: it.favicon, alt: '', loading: 'lazy', referrerpolicy: 'no-referrer' });
      im.onerror = () => { im.remove(); fav.style.background = PAL[0]; fav.textContent = initials(it.name); };
      fav.append(im);
    } else { fav.style.background = PAL[it.name.length % PAL.length]; fav.textContent = initials(it.name); }
    const meta = [it.country, it.lang, (it.tags || '').split(',').map(x => x.trim()).filter(Boolean).slice(0, 3).join(', '),
      [it.codec, it.bitrate ? it.bitrate + ' кбіт/с' : ''].filter(Boolean).join(' ')].filter(Boolean).join(' · ');
    const el = h('div', { class: 'st', style: { minHeight: '60px' } }, fav,
      h('div', { class: 'tx', onclick: () => pl.click(), title: url },
        h('div', { class: 'nm' }, it.name, !plays ? h('span', { class: 'pill bad', style: { marginLeft: '8px' } }, `${it.codec} — радіо не зіграє`) : null),
        h('div', { class: 'ur' }, meta || url)),
      h('div', { class: 'act', style: { alignItems: 'center', gap: '6px' } },
        it.homepage && /^https?:/.test(it.homepage) ? h('a', { class: 'btn sm ghost ico', href: it.homepage, target: '_blank', rel: 'noopener', title: 'Сайт станції' }, ic('globe')) : null,
        pl, add));
    setAdded(have());
    return el;
  }

  async function fetchRb(off) {
    const p = { limit: 60, offset: off, order: F.order, reverse: F.order === 'name' ? 'false' : 'true', hidebroken: F.working ? 'true' : 'false' };
    if (F.name) p.name = F.name;
    if (F.country) p.countrycode = F.country;
    if (F.lang) { p.language = F.lang; p.languageExact = 'true'; }
    if (F.tag) p.tag = F.tag;
    if (F.codec) { p.codec = F.codec; p.codecExact = 'true'; }
    if (F.br) p.bitrateMin = F.br;
    const got = await rbGet('stations/search', p);
    return {
      raw: got.length,
      rows: got.map(x => ({ uuid: x.stationuuid, name: (x.name || '').trim(), url: (x.url_resolved || x.url || '').trim(), favicon: x.favicon, homepage: x.homepage,
        country: x.countrycode ? countryName(x.countrycode, x.country) : x.country,
        lang: (x.language || '').split(',').map(l => l.trim()).filter(Boolean).slice(0, 2).map(l => (FIND.langMap && FIND.langMap.get(l)) || capital(l)).join(' / '),
        tags: x.tags, codec: x.codec, bitrate: x.bitrate })) };
  }

  async function fetchXiph(off) {
    const all = await xiphLoad();
    const n = F.name.toLowerCase(), g = F.tag.toLowerCase();
    let r = all.filter(x => (!n || x.name.toLowerCase().includes(n)) && (!g || x.tags.toLowerCase().includes(g)) &&
      (!F.codec || x.codec === F.codec) && (!F.br || x.bitrate >= F.br) && (!F.plays || radioPlays(x.codec)));
    r = F.order === 'bitrate' ? r.sort((a, b) => b.bitrate - a.bitrate) : r.sort((a, b) => a.name.localeCompare(b.name, 'uk'));
    FIND.xiphTotal = r.length;
    return { raw: Math.max(0, Math.min(60, r.length - off)), rows: r.slice(off, off + 60) };
  }

  async function run(append) {
    F.name = q.value.trim(); F.tag = tag.value.trim(); F.country = country.value; F.lang = lang.value;
    F.codec = codec.value; F.br = +br.value; F.order = order.value; keep();
    const my = ++token;
    if (!append) {
      offset = 0; items = []; rowsRedraw = [];
      list.textContent = ''; more.textContent = '';
      list.append(h('div', { class: 'none' }, F.src === 'xiph' && !FIND.xiph ? 'Вантажу каталог Icecast (близько 9 МБ)…' : 'Шукаю…'));
      info.textContent = '';
    }
    let res;
    try { res = await (F.src === 'rb' ? fetchRb(offset) : fetchXiph(offset)); }
    catch (e) { if (my !== token) return; list.textContent = ''; list.append(h('div', { class: 'none' }, 'Не вдалося: ' + e.message)); return; }
    if (my !== token) return;
    if (!append) list.textContent = '';
    offset += 60;
    const hv = new Set(items.map(x => x.url));
    const fresh = res.rows.filter(x => /^https?:\/\//.test(x.url) && bytes(x.url) <= 160 && x.name && !hv.has(x.url) && (!F.plays || radioPlays(x.codec)));
    items.push(...fresh);
    fresh.forEach(it => list.append(rowEl(it)));
    const shown = items.length;
    info.textContent = F.src === 'xiph'
      ? `Знайдено ${FIND.xiphTotal}${F.plays ? ' (показую ті, що зіграє радіо)' : ''}`
      : shown ? `Показано ${shown}` : '';
    if (!shown) list.append(h('div', { class: 'none' }, 'Нічого не знайдено. Спробуйте коротшу назву або приберіть частину умов.'));
    more.textContent = '';
    if (res.raw >= 60) more.append(h('div', { class: 'none', style: { padding: '14px' } }, btn('Показати ще', 'down', () => { more.textContent = ''; run(true); }, 'sm')));
  }

  srcUi();
  /*  пішли з розділу — прослуховування вимкнути (відчеплений <audio> грав би далі)  */
  window.addEventListener('hashchange', () => { audio.pause(); audio.removeAttribute('src'); }, { once: true });
  q.addEventListener('keydown', e => { if (e.key === 'Enter') run(); });
  tag.addEventListener('keydown', e => { if (e.key === 'Enter') run(); });
  for (const s of [country, lang, codec, br, order]) s.addEventListener('change', () => run());
  live(what => { if (what === 'pl') { drawDirty(); redrawRows(); } });
  try { await loadEditor(); } catch (e) {}
  drawDirty();
  try {
    await findLists();
    const ua = FIND.countries.find(c => c.code === 'UA');
    if (ua) country.append(h('option', { value: 'UA' }, `${ua.name} (${ua.n})`), h('option', { disabled: true }, '──────────'));
    FIND.countries.forEach(c => country.append(h('option', { value: c.code }, `${c.name} (${c.n})`)));
    const uk = FIND.languages.find(l => l.key === 'ukrainian');
    if (uk) lang.append(h('option', { value: uk.key }, `${uk.name} (${uk.n})`), h('option', { disabled: true }, '──────────'));
    FIND.languages.forEach(l => lang.append(h('option', { value: l.key }, `${l.name} (${l.n})`)));
    FIND.tags.forEach(t => tagList.append(h('option', { value: t })));
  } catch (e) { if (F.src === 'rb') { list.textContent = ''; list.append(h('div', { class: 'none' }, 'Не вдалося: ' + e.message)); } }
  country.value = F.country; lang.value = F.lang;
  if (!page.isConnected) return;
  run();
};

function pickFavSlot(s) {
  if (!S) return;
  const grid = h('div', { class: 'favs' });
  const d = dialog('В обране', h('div', null, h('p', { class: 'note', style: { marginTop: 0 } }, `Куди поставити «${s.name}»?`), grid), [btn('Скасувати', null, () => d.close(), 'ghost')]);
  S.fav.forEach((f, i) => grid.append(h('button', { type: 'button', class: 'fav' + (f.n ? '' : ' empty'), style: { cursor: 'pointer' }, onclick: async () => {
    await setx({ favPut: `${i}\t${s.name}\t${s.url}` }); d.close(); toast(`«${s.name}» — у кнопці ${i + 1}`); pollState();
  } }, h('span', { class: 'n' }, f.n || 'порожньо'), h('span', { class: 'k' }, String(i + 1)))));
}

/* ---------------------------------------------------------------- обране */
VIEWS.fav = page => {
  const grid = h('div', { class: 'list' });
  const mainSw = sw(true, v => setx({ favHide: v ? 0 : 1 }));
  page.append(h('p', { class: 'lead' }, 'Шість кнопок швидкого вибору — ті самі, що в меню радіо «обране». Станцію можна поставити з цього розділу, зі списку станцій (зірочка) або на самому радіо.'),
    card(null, row('На головному екрані радіо', 'рядок із шести логотипів під годинником: дотик — і станція грає', mainSw)),
    grid);
  live(() => {
    if (!S) return;
    mainSw.firstChild.checked = !S.favHide;
    const sig = JSON.stringify(S.fav) + S.favOn + W.name;
    if (grid.dataset.sig === sig) return;
    grid.dataset.sig = sig; grid.textContent = '';
    S.fav.forEach((f, i) => {
      const sel = h('select', { 'aria-label': 'Станція зі списку' }, h('option', { value: '' }, 'Вибрати зі списку…'), PL.map((s, k) => h('option', { value: k }, `${k + 1}. ${s.name}`)));
      sel.onchange = async () => { const s = PL[+sel.value]; if (!s) return; await setx({ favPut: `${i}\t${s.name}\t${s.url}` }); pollState(); };
      grid.append(h('div', { class: 'st' + (S.favOn === i ? ' on' : '') },
        h('span', { class: 'no', style: { display: 'block', fontSize: '15px', color: 'var(--acc)' } }, i + 1),
        f.n ? logoEl(f.n, f.u) : h('div', { class: 'logo', style: { border: '1px dashed #444' } }),
        h('div', { class: 'tx', onclick: () => f.n && setx({ favPlay: i }) }, h('div', { class: 'nm' }, f.n || 'порожньо'), h('div', { class: 'ur' }, f.u || 'кнопка вільна')),
        h('div', { class: 'act', style: { alignItems: 'center', gap: '6px', flexWrap: 'wrap', justifyContent: 'flex-end' } },
          f.n ? ibtn('play', 'Увімкнути', () => setx({ favPlay: i }), 'ghost sm') : null,
          W.mode === 0 && W.name && !isSerm() ? btn('Поточна', null, async () => { await setx({ favSet: i }); pollState(); }, 'sm ghost') : null,
          h('div', { style: { width: '170px' } }, sel),
          f.n ? ibtn('x', 'Звільнити кнопку', async () => { await setx({ favClear: i }); pollState(); }, 'ghost sm') : null)));
    });
  });
};

/* ---------------------------------------------------------------- проповіді */
VIEWS.sermons = page => {
  const list = h('div', { class: 'list' }, h('div', { class: 'none' }, 'Завантажую з сайту…'));
  const q = h('input', { type: 'search', placeholder: 'Пошук за назвою, місцем Писання', 'aria-label': 'Пошук' });
  const who = h('select', { 'aria-label': 'Проповідник' }, h('option', { value: '' }, 'Усі проповідники'));
  const info = h('span', { class: 'note', style: { margin: 0 } });
  page.append(
    h('p', { class: 'lead' }, 'Увесь архів проповідей із сайту церкви, від найновіших. Натисніть, щоб слухати на радіо.'),
    h('div', { class: 'tools' }, h('div', { class: 'bar' }, h('div', { class: 'search' }, ic('search'), q), h('div', { style: { width: '240px', maxWidth: '100%' } }, who),
      btn('Оновити', 'refresh', () => { setx({ sermLoad: 1 }); data = null; list.textContent = ''; list.append(h('div', { class: 'none' }, 'Оновлюю…')); setTimeout(load, 2000); }, 'sm')),
      h('div', { style: { marginTop: '8px' } }, info)),
    list);
  let data = null, t = 0, limit = 60;
  async function load() {
    clearTimeout(t);
    try { data = await api('/api/sermons'); } catch (e) { list.textContent = ''; list.append(h('div', { class: 'none' }, 'Радіо не відповіло: ' + e.message)); t = setTimeout(load, 4000); return; }
    if (S) S.sermSite = data.site;
    if (data.load && !data.items.length) {
      list.textContent = ''; list.append(h('div', { class: 'none' }, data.err ? 'Сайт не відповідає: ' + data.err : `Радіо завантажує архів із сайту…${data.got ? ' ' + data.got : ''}`));
      t = setTimeout(load, 1500); data = null; return;
    }
    /*  проповідники — від тих, у кого найбільше проповідей  */
    const cnt = {};
    data.items.forEach(it => { const p = it.p || ''; cnt[p] = (cnt[p] || 0) + 1; });
    const keep = who.value;
    who.textContent = '';
    who.append(h('option', { value: '' }, `Усі проповідники (${data.items.length})`),
      Object.keys(cnt).sort((x, y) => cnt[y] - cnt[x]).map(p => h('option', { value: p || '—' }, `${p || 'без імені'} (${cnt[p]})`)));
    who.value = keep;
    draw();
  }
  function draw() {
    if (!data) return;
    const cur = isSerm() ? S.serm.i : -1;
    const f = q.value.trim().toLowerCase(), w = who.value;
    const shown = [];
    data.items.forEach((it, i) => {
      if (w && (it.p || '—') !== w) return;
      if (f && !(it.t.toLowerCase().includes(f) || (it.p || '').toLowerCase().includes(f) || (it.d || '').includes(f))) return;
      shown.push(i);
    });
    info.textContent = shown.length === data.items.length ? `${data.items.length} ${plural(data.items.length, 'проповідь', 'проповіді', 'проповідей')}` : `Знайдено ${shown.length} з ${data.items.length}`;
    list.textContent = '';
    shown.slice(0, limit).forEach(i => {
      const it = data.items[i];
      const cv = h('div', { class: 'cv' });
      if (it.c) cv.append(h('img', { src: (/^https?:/.test(it.c) ? '' : data.site) + it.c, alt: '', loading: 'lazy' }));
      const date = it.d ? it.d.split('-').reverse().join('.') : '';
      list.append(h('div', { class: 'srm' + (i === cur ? ' on' : ''), style: { cursor: 'pointer' }, onclick: () => { setx({ serm: i }); toast('Вмикаю проповідь…'); } },
        cv,
        h('div', null, h('div', { class: 't' }, it.t), h('div', { class: 'p' }, [it.p, date, it.dur ? mmss(it.dur) : ''].filter(Boolean).join(' · '))),
        h('div', { class: 'go' }, i === cur ? h('span', { class: 'pill acc' }, W.playing ? 'грає' : 'пауза') : ibtn('play', 'Слухати на радіо', null, 'ghost'))));
    });
    if (shown.length > limit) list.append(h('div', { class: 'none' }, btn(`Показати ще (${shown.length - limit})`, 'down', () => { limit += 100; draw(); })));
    if (!shown.length) list.append(h('div', { class: 'none' }, data.items.length ? 'Нічого не знайдено.' : (data.err ? 'Сайт не відповідає: ' + data.err : 'Проповідей немає.')));
  }
  let qt = 0;
  q.addEventListener('input', () => { clearTimeout(qt); qt = setTimeout(() => { limit = 60; draw(); }, 150); });
  who.addEventListener('change', () => { limit = 60; draw(); });
  load();
  let last = '';
  live(() => { const s = (isSerm() ? S.serm.i : -1) + '|' + W.playing; if (s !== last) { last = s; draw(); } });
};

/* ---------------------------------------------------------------- запис і картка */
VIEWS.records = page => {
  const recBox = h('div');
  const list = h('div', { class: 'list' }, h('div', { class: 'none' }, 'Читаю картку…'));
  const info = h('p', { class: 'note' });
  const audio = h('audio', { controls: true, preload: 'none', style: { width: '100%', marginTop: '10px' }, hidden: true });
  page.append(card('Запис ефіру', recBox),
    h('section', { class: 'card', style: { padding: 0, overflow: 'hidden' } },
      h('h2', { style: { padding: '16px 16px 0' } }, 'Записи на картці', h('span', { class: 'sp' }), btn('Оновити', 'refresh', load, 'sm ghost')),
      h('div', { style: { padding: '0 16px' } }, info, audio), list),
    card('Картка як джерело', h('div', { class: 'bar' }, h('span', { style: { flex: 1 } }, 'Слухати музику з картки пам\'яті на радіо. Записи з\'являються там само.'), btn('Грати з картки', 'sd', () => { send('newmode=1'); location.hash = '#/player'; }))));
  async function load() {
    let j;
    try { j = await api('/api/records'); } catch (e) { list.textContent = ''; list.append(h('div', { class: 'none' }, e.message)); return; }
    list.textContent = '';
    if (j.err) { info.textContent = ''; list.append(h('div', { class: 'none' }, j.err)); return; }
    info.textContent = `Вільно ${size(j.free)} з ${size(j.total)}.`;
    const items = (j.items || []).sort((a, b) => b.f.localeCompare(a.f));
    if (!items.length) { list.append(h('div', { class: 'none' }, 'Записів ще немає.')); return; }
    for (const it of items) {
      const m = it.f.match(/^(\d{4})(\d\d)(\d\d)-(\d\d)(\d\d)(\d\d)_(.*)\.(\w+)$/);
      const title = m ? m[7].replace(/_/g, ' ') : it.f;
      const when = m ? `${m[3]}.${m[2]}.${m[1]} ${m[4]}:${m[5]}` : '';
      const busy = S && S.rec.on && S.rec.file === it.f;
      list.append(h('div', { class: 'st' },
        h('div', { class: 'logo', style: { background: 'var(--pan2)', color: 'var(--dim)' } }, ic('file')),
        h('div', { class: 'tx' }, h('div', { class: 'nm' }, title, busy ? h('span', { class: 'badge' }, 'пишеться') : null), h('div', { class: 'ur' }, [when, size(it.s), m ? m[8].toUpperCase() : ''].filter(Boolean).join(' · '))),
        h('div', { class: 'act' },
          ibtn('play', 'Послухати в браузері', () => { audio.hidden = false; audio.src = '/api/rec?f=' + encodeURIComponent(it.f); audio.play().catch(() => toast('Браузер не відтворює цей формат — завантажте файл', true)); }, 'ghost sm'),
          h('a', { class: 'btn ico sm ghost', href: '/api/rec?dl=1&f=' + encodeURIComponent(it.f), download: it.f, title: 'Завантажити', 'aria-label': 'Завантажити' }, ic('download')),
          busy ? null : ibtn('trash', 'Видалити', async () => {
            if (!await confirmBox('Видалити запис?', `${title} ${when} (${size(it.s)}) буде стерто з картки назавжди.`, 'Видалити', true)) return;
            audio.pause(); await setx({ recDel: it.f }); setTimeout(load, 600);
          }, 'ghost sm'))));
    }
  }
  load();
  let lastOn = null;
  live(() => {
    if (!S) return;
    const serm = isSerm();
    recBox.textContent = '';
    if (S.rec.on) {
      recBox.append(h('div', { class: 'bar' }, h('span', { style: { fontSize: '18px' } }, h('i', { class: 'recdot' }), mmss(S.rec.sec)), h('span', { class: 'pill' }, size(S.rec.bytes)), h('span', { class: 'sp' }), btn('Зупинити запис', 'stop', () => setx({ rec: 0 }), 'acc')),
        h('p', { class: 'note' }, 'Файл: ' + S.rec.file + '. Запис зупиниться сам, якщо перемкнути станцію.'));
    } else {
      recBox.append(h('div', { class: 'bar' }, h('span', { style: { flex: 1 } }, W.playing && W.mode === 0 && !serm ? 'Записати те, що грає: ' + W.name : 'Увімкніть радіостанцію, щоб записати ефір.'), btn('Почати запис', 'rec', () => setx({ rec: 1 }), 'acc')),
        h('p', { class: 'note' }, 'Потік пишеться на картку як є — MP3 лишається MP3, AAC — AAC. Проповіді й музику з картки не записуємо.'));
    }
    if (lastOn !== null && lastOn !== S.rec.on) setTimeout(load, 800);
    lastOn = S.rec.on;
  });
};

/* ---------------------------------------------------------------- будильник і сон */
const HALF = Array.from({ length: 48 }, (_, i) => [i, `${pad2(i >> 1)}:${i & 1 ? '30' : '00'}`]);
function selectEl(items, cur, onchange) {
  const s = h('select', null, items.map(([v, l]) => h('option', { value: v, selected: String(v) === String(cur) }, l)));
  s.onchange = () => onchange(s.value);
  return s;
}
VIEWS.alarm = page => {
  const sl = h('div'), al = h('div');
  page.append(card('Таймер сну', sl), card('Будильник', al));
  const sleepSeg = seg([[0, 'Вимк'], [15, '15 хв'], [30, '30 хв'], [60, '1 год'], [90, '1,5 год']], -1, v => setx({ sleep: v }));
  const own = h('input', { type: 'number', min: 1, max: 600, placeholder: 'хв', style: { width: '90px' } });
  const sleepNote = h('p', { class: 'note' });
  sl.append(sleepSeg, h('div', { class: 'bar', style: { marginTop: '12px' } }, h('span', null, 'Або своє:'), own, btn('Поставити', null, () => { const v = parseInt(own.value); if (v > 0) setx({ sleep: Math.min(600, v) }); }, 'sm')), sleepNote);
  const time = h('input', { type: 'time', style: { width: '130px' } });
  const onSw = sw(false, v => setx({ alarmOn: v ? 1 : 0 }));
  const days = seg([[0, 'Щодня'], [1, 'Будні']], 0, v => setx({ alarmDays: v }));
  const aNote = h('p', { class: 'note' });
  time.addEventListener('change', () => { const [hh, mm] = time.value.split(':').map(Number); if (!isNaN(hh)) setx({ alarmH: hh, alarmM: mm, alarmOn: 1 }); });
  al.append(row('Будильник', 'грає остання радіостанція, гучність наростає 30 секунд', onSw),
    row('Час', null, time), row('Дні', null, days), aNote);
  live(() => {
    if (!S) return;
    sleepSeg.set(S.sleep.set);
    sleepNote.textContent = S.sleep.set ? `Радіо вимкнеться через ${mmss(S.sleep.left)}; останні 20 секунд звук плавно стихає, екран гасне.` : 'Таймер не стоїть.';
    const a = S.alarm;
    onSw.firstChild.checked = !!a.on;
    if (!held(time)) time.value = `${pad2(a.h)}:${pad2(a.m)}`;
    days.set(a.days);
    aNote.textContent = !a.on ? 'Будильник вимкнено.' : a.ring ? 'Будильник дзвонить зараз.' : a.in < 0 ? 'Радіо ще не знає точного часу.' : `Задзвонить через ${Math.floor(a.in / 60)} год ${pad2(a.in % 60)} хв${W.mode === 0 && W.name ? '. Заграє «' + W.name + '»' : ''}.`;
  });
};

/* ---------------------------------------------------------------- екран */
VIEWS.screen = page => {
  let brT = 0, nlT = 0;
  const brOut = h('output');
  const br = range(0, 100, 50, v => { brOut.textContent = v + '%'; clearTimeout(brT); brT = setTimeout(() => setx({ bright: v }), 120); });
  const nOn = sw(false, v => setx({ nightOn: v ? 1 : 0 }));
  const nFrom = selectEl(HALF, 44, v => setx({ nightFrom: v }));
  const nTo = selectEl(HALF, 14, v => setx({ nightTo: v }));
  const nlOut = h('output');
  const nl = range(0, 100, 10, v => { nlOut.textContent = v ? v + '%' : 'гасне'; clearTimeout(nlT); nlT = setTimeout(() => setx({ nightLevel: v }), 150); });
  const save = seg([[0, 'Вимк'], [1, '10 с'], [2, '15 с'], [3, '30 с'], [4, '60 с']], 0, v => setx({ batSave: v }));
  const led = seg([[0, 'Вимк'], [1, 'Стан'], [2, 'Музика']], 1, v => setx({ ledMode: v }));
  const nNote = h('p', { class: 'note' });
  page.append(
    card('Яскравість', h('div', { class: 'rng' }, br, brOut), h('p', { class: 'note' }, 'Денна яскравість екрана. Змінюється одразу.')),
    card('Нічний режим', row('Приглушувати вночі', 'за розкладом; дотик ненадовго повертає денну яскравість', nOn),
      row('Від', null, h('div', { style: { width: '110px' } }, nFrom)), row('До', null, h('div', { style: { width: '110px' } }, nTo)),
      h('div', { class: 'fld', style: { marginTop: '8px' } }, h('span', null, 'Нічна яскравість (0 — екран гасне)'), h('div', { class: 'rng' }, nl, nlOut)), nNote),
    card('Економія батареї', save, h('p', { class: 'note' }, 'Коли радіо працює від акумулятора, екран після стількох секунд без дотиків плавно пригасає. Перший дотик лише будить його.')),
    card('Світлодіод', led, h('p', { class: 'note' }, '«Стан» — колір показує, що робить радіо; «Музика» — блимає в такт. Під час запису — червоний.')),
    card('Додатково', h('div', { id: 'scrx' })));
  send('getscreen=1');
  const x = $('#scrx');
  let xs = '';
  live(what => {
    if (S) {
      if (!held(br)) { br.value = S.bright; fillRange(br); brOut.textContent = S.bright + '%'; }
      nOn.firstChild.checked = !!S.night.on;
      if (!held(nFrom)) nFrom.value = S.night.from;
      if (!held(nTo)) nTo.value = S.night.to;
      if (!held(nl)) { nl.value = S.night.level; fillRange(nl); nlOut.textContent = S.night.level ? S.night.level + '%' : 'гасне'; }
      nNote.textContent = S.night.on ? (S.night.act ? 'Зараз діє нічний режим.' : 'Зараз денний режим.') : '';
      save.set(S.save); led.set(S.led);
    }
    if ('flip' in C) {
      const s = ['flip', 'inv', 'tsf', 'scre', 'scrt', 'scrb', 'scrpe', 'scrpt', 'scrpb'].map(k => C[k]).join('|');
      if (s === xs) return; xs = s;
      x.textContent = '';
      const num = (k, cmd, min, max) => { const i = h('input', { type: 'number', min, max, value: C[k], style: { width: '90px' } }); i.onchange = () => send(`${cmd}=${i.value}`); return i; };
      x.append(
        row('Перевернути екран', null, sw(+C.flip, v => send('flipscreen=' + (v ? 1 : 0)))),
        row('Інверсія кольорів', null, sw(+C.inv, v => send('invertdisplay=' + (v ? 1 : 0)))),
        row('Перевернути сенсор', 'якщо дотики потрапляють не туди', sw(+C.tsf, v => send('fliptouch=' + (v ? 1 : 0)))),
        row('Гасити екран, коли не грає', 'дотик засвітить', sw(+C.scre, v => send('screensaverenabled=' + (v ? 1 : 0)))),
        row('— через, секунд', null, num('scrt', 'screensavertimeout', 5, 65520)),
        row('Гасити екран під час відтворення', null, sw(+C.scrpe, v => send('screensaverplayingenabled=' + (v ? 1 : 0)))),
        row('— через, хвилин', null, num('scrpt', 'screensaverplayingtimeout', 1, 1080)));
    }
  });
};

/* ---------------------------------------------------------------- звук */
const EQ_HZ = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000];
const EQ_LBL = ['31', '62', '125', '250', '500', '1к', '2к', '4к', '8к', '16к'];
const EQ_PRESETS = ['свій', 'рівно', 'голос', 'музика', 'бас', 'ніч', 'яскраво', 'тепло'];
const MIC_ACTS = [[2, 'пауза / грати'], [3, 'наступна станція'], [4, 'попередня станція'], [5, 'гучніше'], [6, 'тихіше'], [7, 'екран: погасити / розбудити'], [8, 'обране 1'], [1, 'нічого']];

/*  Графік АЧХ: сітка ±18 дБ, 20 Гц … 20 кГц у логарифмі, крива — бурштином,
    замір кімнати — бірюзовими точками.  */
function eqPlot(cv, resp, sweep, room) {
  const dpr = window.devicePixelRatio || 1, W0 = cv.clientWidth || 600, H0 = cv.clientHeight || 180;
  cv.width = W0 * dpr; cv.height = H0 * dpr;
  const g = cv.getContext('2d'); g.scale(dpr, dpr);
  const L = 34, R = 8, T = 8, B = 20, w = W0 - L - R, hh = H0 - T - B;
  const X = f => L + Math.log(f / 20) / Math.log(1000) * w, Y = db => T + (18 - Math.max(-18, Math.min(18, db))) / 36 * hh;
  g.clearRect(0, 0, W0, H0);
  g.font = '11px system-ui, sans-serif'; g.fillStyle = '#8c8c8c'; g.strokeStyle = '#262626'; g.lineWidth = 1;
  for (const db of [-18, -12, -6, 0, 6, 12, 18]) { g.beginPath(); g.moveTo(L, Y(db)); g.lineTo(L + w, Y(db)); g.strokeStyle = db ? '#262626' : '#444'; g.stroke(); g.fillText((db > 0 ? '+' : '') + db, 4, Y(db) + 4); }
  [[50, '50'], [100, '100'], [200, '200'], [500, '500'], [1000, '1к'], [2000, '2к'], [5000, '5к'], [10000, '10к']].forEach(([f, t]) => { g.beginPath(); g.moveTo(X(f), T); g.lineTo(X(f), T + hh); g.strokeStyle = '#1f1f1f'; g.stroke(); g.fillText(t, X(f) - 8, H0 - 5); });
  if (sweep && sweep.length) {
    const mid = sweep.filter(p => p[0] >= 500 && p[0] <= 4000), ref = mid.length ? mid.reduce((a, p) => a + p[1], 0) / mid.length / 10 : 0;
    g.fillStyle = '#00d7d7';
    for (const p of sweep) { g.beginPath(); g.arc(X(p[0]), Y(p[1] / 10 - ref), 2.5, 0, 7); g.fill(); }
  }
  if (resp && resp.length) {
    g.beginPath(); g.lineWidth = 2.5; g.strokeStyle = '#e6d25a';
    resp.forEach((p, i) => { const x = X(p[0]), y = Y(p[1] / 10); i ? g.lineTo(x, y) : g.moveTo(x, y); });
    g.stroke();
  }
}

VIEWS.sound = page => {
  const presets = seg(EQ_PRESETS.slice(1).map((n, i) => [i + 1, n]), -1, v => setx({ eqPreset: v }));
  const on = sw(true, v => setx({ eqOn: v ? 1 : 0 }));
  const cv = h('canvas', { class: 'eqplot' });
  const sl = [], outs = [];
  const geq = h('div', { class: 'geq' });
  EQ_HZ.forEach((f, i) => {
    const o = h('output', null, '0');
    let t = 0;
    const r = range(-12, 12, 0, v => { o.textContent = (v > 0 ? '+' : '') + v; clearTimeout(t); t = setTimeout(() => { setx({ eqBand: i + ':' + v }); plotSoon(); }, 150); });
    r.setAttribute('orient', 'vertical');
    r.setAttribute('aria-label', EQ_LBL[i] + ' Гц');
    sl.push(r); outs.push(o);
    geq.append(h('div', { class: 'col' }, o, r, h('small', null, EQ_LBL[i])));
  });
  const preNote = h('p', { class: 'note' });
  const guard = seg([[0, 'Вимк'], [1, 'М\'який'], [2, 'Сильний']], -1, v => { setx({ eqGuard: v }); plotSoon(); });
  const vb = seg([[0, 'Вимк'], [1, '1'], [2, '2'], [3, '3']], -1, v => setx({ vbass: v }));
  const loud = seg([[0, 'Вимк'], [1, 'М\'яка'], [2, 'Сильна']], -1, v => { setx({ eqLoud: v }); plotSoon(); });
  const balOut = h('output');
  let balT = 0;
  const bal = range(-16, 16, 0, v => { balOut.textContent = v ? (v < 0 ? 'ліворуч ' + -v : 'праворуч ' + v) : 'по центру'; clearTimeout(balT); balT = setTimeout(() => setx({ bal: v }), 150); });
  const roomBtn = btn('Зміряти', 'refresh', () => { if (S && (S.snd.rst === 1 || S.snd.rst === 2)) setx({ roomStop: 1 }); else setx({ roomTune: 1 }); }, 'acc');
  const roomOn = sw(false, v => { setx({ eqRoomOn: v ? 1 : 0 }); plotSoon(); });
  const roomClr = btn('Скинути поправку', 'x', () => { setx({ roomClear: 1 }); plotSoon(); }, 'sm ghost');
  const roomBar = h('div', { class: 'prog' }, h('i'));
  const roomMsg = h('p', { class: 'note' });
  const roomBands = h('div', { class: 'rbands' });
  const ctl = h('div');
  page.append(
    card('Еквалайзер', h('div', { class: 'bar', style: { justifyContent: 'space-between' } }, presets, h('label', { class: 'bar' }, h('span', { class: 'note', style: { margin: 0 } }, 'увімкнено'), on)),
      cv, geq, preNote),
    card('Обробка',
      row('Захист динаміка', 'зрізає низ, якого маленький динамік не відтворює, а лише хрипить', guard),
      row('Віртуальний бас', 'бас, що зрізано, передається гармоніками — вухо саме «добудовує» низ', vb),
      row('Тонкомпенсація', 'на тихій гучності трохи підіймає низ і верх, як їх чує вухо', loud),
      h('div', { class: 'fld', style: { marginTop: '8px' } }, h('span', null, 'Баланс'), h('div', { class: 'rng' }, bal, balOut)),
      h('p', { class: 'note' }, 'Захист і тонкомпенсація — для вбудованого динаміка; для зовнішнього ЦАП захист вимикається сам.')),
    card('Під кімнату',
      h('p', { class: 'note', style: { marginTop: 0 } }, 'Радіо зіграє тони від 63 Гц до 16 кГц і послухає себе своїм мікрофоном. Спершу перевіряє, чи мікрофон і динамік не спотворюють звук (інакше зменшує підсилення чи гучність тонів), тоді ставить поправку й міряє ще раз — уже з нею, доправляючи те, що лишилось. Якщо рівнішим звук не став, поправка не вмикається. Близько хвилини; у кімнаті має бути тихо, грати в цей час нічого не буде.'),
      h('div', { class: 'bar', style: { marginTop: '10px' } }, roomBtn, roomClr), roomBar, roomMsg, roomBands,
      row('Застосовувати поправку', 'бірюзові точки на графіку — що почув мікрофон', roomOn)),
    card('Гучність', ctl),
    card('Аудіовихід', h('div', { class: 'bar' }, h('span', { id: 'dacnow', style: { flex: 1 } }), h('a', { class: 'btn sm', href: '#/dev' }, 'Змінити')), h('p', { class: 'note' }, 'Зовнішній ЦАП чи підсилювач під\'єднують у розділі «Розробник» — там і схеми.')));
  send('getcontrols=1'); send('getsystem=1');
  let plotT = 0, last = null;
  async function plot() { try { last = await api('/api/eq'); eqPlot(cv, last.resp, last.sweep); } catch (e) {} }
  function plotSoon() { clearTimeout(plotT); plotT = setTimeout(plot, 500); }
  window.addEventListener('resize', plotSoon);
  plot();
  let cs = '', rst = -1, sig = '';
  live(() => {
    if (S && S.snd) {
      const d = S.snd;
      d.eq.forEach((v, i) => { if (!held(sl[i])) { sl[i].value = v; fillRange(sl[i]); outs[i].textContent = (v > 0 ? '+' : '') + v; } });
      presets.set(d.preset);
      on.firstChild.checked = !!d.on;
      geq.classList.toggle('off', !d.on);
      guard.set(d.guard); vb.set(d.vb); loud.set(d.loud);
      if (!held(bal)) { bal.value = d.bal; fillRange(bal); balOut.textContent = d.bal ? (d.bal < 0 ? 'ліворуч ' + -d.bal : 'праворуч ' + d.bal) : 'по центру'; }
      roomOn.firstChild.checked = !!d.roomOn;
      const busy = d.rst === 1 || d.rst === 2;
      roomBtn.lastChild.textContent = busy ? 'Зупинити' : 'Зміряти';
      roomBar.hidden = !busy; roomBar.firstChild.style.width = d.rpr + '%';
      roomMsg.textContent = busy ? d.rmsg + '… ' + d.rpr + '%' : d.rst === 3 ? 'Готово — ' + d.rmsg + '.' : d.rst === 4 ? 'Не вийшло: ' + d.rmsg : '';
      const has = d.room.some(v => v);
      roomClr.hidden = !has;
      roomBands.textContent = '';
      if (has) d.room.forEach((v, i) => roomBands.append(h('span', { class: v ? '' : 'z' }, EQ_LBL[i] + ' ', h('b', null, (v > 0 ? '+' : '') + v))));
      preNote.textContent = `Попереднє ослаблення ${(-d.pre10 / 10).toFixed(1).replace(".", ",")} дБ — щоб підйоми не перевантажували звук · обробка: ${(d.us100 / 100).toFixed(1).replace(".", ",")} мкс на відлік`;
      const s2 = d.eq.join() + d.on + d.guard + d.loud + d.roomOn + d.room.join();
      if (s2 !== sig) { sig = s2; plotSoon(); }
      if (d.rst !== rst) { if (rst === 2 && d.rst >= 3) plotSoon(); rst = d.rst; }
      $('#dacnow').textContent = 'Зараз: ' + dacOf(S.dev.dac).n + ' — ' + dacOf(S.dev.dac).k;
    }
    const s = [C.vols, C.sst].join('|');
    if ('vols' in C && s !== cs) {
      cs = s; ctl.textContent = '';
      const v = h('input', { type: 'number', min: 1, max: 10, value: C.vols, style: { width: '90px' } }); v.onchange = () => send('volsteps=' + v.value);
      ctl.append(row('Крок гучності', 'на скільки змінюється гучність одним натисканням', v));
      if ('sst' in C) ctl.append(row('Грати після ввімкнення', 'якщо радіо грало, коли його вимкнули', sw(+C.sst, x => send('smartstart=' + (x ? 1 : 0)))));
    }
  });
};

/* ---------------------------------------------------------------- мікрофон */
VIEWS.mic = page => {
  const on = sw(false, v => setx({ micOn: v ? 1 : 0 }));
  const gain = seg([[3, 'Низька'], [0, 'Середня'], [7, 'Висока'], [8, 'Макс']], -1, v => setx({ micGain: v }));
  const play = sw(false, v => setx({ micPlay: v ? 1 : 0 }));
  const meter = h('div', { class: 'meter' }, h('i'), h('b'));
  const heard = h('p', { class: 'note' });
  const kind = (k, title, sub) => {
    const o = sw(false, v => setx({ [k + 'On']: v ? 1 : 0 }));
    const sens = seg([[1, 'Низька'], [0, 'Середня'], [2, 'Висока']], -1, v => setx({ [k + 'Sens']: v }));
    const a2 = selectEl(MIC_ACTS, 2, v => setx({ [k + '2']: v }));
    const a3 = selectEl(MIC_ACTS, 3, v => setx({ [k + '3']: v }));
    const el = card(title, row('Увімкнено', sub, o), row('Чутливість', null, sens),
      row('Двічі', null, h('div', { style: { width: '220px' } }, a2)), row('Тричі', null, h('div', { style: { width: '220px' } }, a3)));
    el.upd = m => { o.firstChild.checked = !!m[k + 'On']; sens.set(m[k + 'Sens']); if (!held(a2)) a2.value = m[k + '2']; if (!held(a3)) a3.value = m[k + '3']; };
    return el;
  };
  const clap = kind('clap', 'Хлопки', 'хлопніть у долоні двічі чи тричі з рівним кроком');
  const knock = kind('knock', 'Стук по корпусу', 'постукайте пальцем по радіо');
  const ear = sw(false, v => setx({ sleepEar: v ? 1 : 0 }));
  const earMin = seg([[5, '5 хв'], [10, '10 хв'], [15, '15 хв'], [30, '30 хв']], -1, v => setx({ sleepEarMin: v }));
  const wake = sw(false, v => setx({ presWake: v ? 1 : 0 }));
  const off = seg([[0, 'Ні'], [5, '5 хв'], [15, '15 хв'], [30, '30 хв']], -1, v => setx({ presOff: v }));
  page.append(
    card('Мікрофон', row('Слухати', 'поки слухає, на екрані горить значок; звук нікуди не передається', on), meter, heard,
      row('Чутливість', null, gain),
      row('Слухати й під час звуку', 'віднімає власний звук радіо, щоб чути кімнату; бере помітну частку процесора', play)),
    clap, knock,
    card('Сон і присутність',
      row('Таймер сну слухає', 'у кімнаті стільки хвилин тихо — радіо затихає, не чекаючи кінця таймера', ear), row('Тиша', null, earMin),
      row('Голос будить екран', 'заговорили поруч — екран прокидається', wake),
      row('Гасити екран, коли тихо', 'нікого не чути й ніхто не торкався стільки хвилин', off)));
  live(() => {
    if (!S || !S.mic) return;
    const m = S.mic;
    on.firstChild.checked = !!m.on; gain.set(m.gain); play.firstChild.checked = !!m.play;
    const p = m.run ? Math.max(0, Math.min(100, (m.lvl + 80) / 60 * 100)) : 0, n = Math.max(0, Math.min(100, (m.noise + 80) / 60 * 100));
    meter.firstChild.style.width = p + '%'; meter.firstChild.classList.toggle('voice', !!m.speech); meter.lastChild.style.left = n + '%';
    heard.textContent = !m.on ? 'Мікрофон вимкнено.' : m.heard ? 'Почуто: ' + m.heard + '.' : m.speech ? 'Чую голос.' : m.aec ? 'Віднімаю власний звук.' : `Рівень ${m.lvl} дБ, фон ${m.noise} дБ.`;
    clap.upd(m); knock.upd(m);
    ear.firstChild.checked = !!m.ear; earMin.set(m.earMin); wake.firstChild.checked = !!m.wake; off.set(m.off);
  });
};

/* ---------------------------------------------------------------- Wi-Fi */
VIEWS.wifi = page => {
  const cur = h('div'), saved = h('div'), scan = h('div');
  page.append(
    card('Зараз', cur),
    card('Збережені мережі', saved, h('p', { class: 'note' }, 'До п\'яти мереж. Після ввімкнення радіо пробує їх по черзі, починаючи з першої.')),
    h('section', { class: 'card' }, h('h2', null, 'Доступні мережі', h('span', { class: 'sp' }), btn('Шукати', 'refresh', doScan, 'sm')), scan,
      h('div', { class: 'bar', style: { marginTop: '12px' } }, btn('Прихована мережа…', 'plus', () => join('', true), 'sm ghost'))));
  let data = null, poll = 0;
  async function load() {
    clearTimeout(poll);
    try { data = await api('/api/wifi'); } catch (e) { return; }
    draw();
    if (data.busy) poll = setTimeout(load, 1000);
  }
  function doScan() { setx({ wifiScan: 1 }); scan.textContent = ''; scan.append(h('div', { class: 'none' }, 'Шукаю мережі…')); setTimeout(load, 1200); }
  function draw() {
    if (!data) return;
    cur.textContent = '';
    cur.append(h('dl', { class: 'kv' }, h('dt', null, 'Мережа'), h('dd', null, data.cur || 'не підключено'),
      S && S.ip ? [h('dt', null, 'Адреса'), h('dd', null, S.ip)] : null, S && S.rssi ? [h('dt', null, 'Сигнал'), h('dd', null, S.rssi + ' дБм')] : null));
    if (data.fail) cur.append(h('p', { class: 'note', style: { color: 'var(--warn)' } }, `Минулого разу не вдалося підключитися до «${data.fail}».`));
    saved.textContent = '';
    if (!data.saved.length) saved.append(h('div', { class: 'none' }, 'Немає збережених мереж.'));
    data.saved.forEach((s, i) => saved.append(h('div', { class: 'row' },
      h('span', { class: 'pill' + (i === 0 ? ' acc' : '') }, i + 1),
      h('div', { class: 'lbl' }, h('b', null, s), s === data.cur ? h('small', null, 'підключено') : null),
      i > 0 ? btn('Першою', 'up', async () => { await setx({ wifiFirst: s }); load(); }, 'sm ghost') : null,
      ibtn('trash', 'Забути', async () => { if (await confirmBox('Забути мережу?', data.saved.length > 1 ? `«${s}» і її пароль буде видалено з радіо.` : `«${s}» і її пароль буде видалено з радіо. Це остання збережена мережа: після вимкнення радіо лишиться без мережі, і вибирати її доведеться на самому радіо.`, 'Забути', true)) { await setx({ wifiForget: s }); load(); } }, 'ghost sm'))));
    scan.textContent = '';
    if (data.busy) scan.append(h('div', { class: 'none' }, 'Шукаю мережі…'));
    else if (!data.scan.length) scan.append(h('div', { class: 'none' }, 'Натисніть «Шукати».'));
    data.scan.forEach(n => scan.append(h('div', { class: 'row', style: { cursor: 'pointer' }, onclick: () => join(n.s, n.e) },
      h('span', { html: sigSvg(n.r), class: 'sig' }),
      h('div', { class: 'lbl' }, h('b', null, n.s), h('small', null, [n.e ? 'з паролем' : 'відкрита', n.s === data.cur ? 'підключено' : data.saved.includes(n.s) ? 'збережена' : ''].filter(Boolean).join(' · '))),
      n.e ? ic('lock', 'rowic') : null)));
  }
  function join(ssid, enc) {
    const ss = h('input', { type: 'text', value: ssid, maxlength: 32, placeholder: 'Назва мережі' });
    const pw = h('input', { type: 'password', maxlength: 64, placeholder: enc ? 'Пароль' : 'без пароля', autocomplete: 'new-password' });
    const show = h('label', { class: 'bar', style: { marginTop: '8px', color: 'var(--dim)', fontSize: '13px', cursor: 'pointer' } }, h('input', { type: 'checkbox', onchange: e => { pw.type = e.target.checked ? 'text' : 'password'; } }), 'показати пароль');
    const err = h('p', { class: 'note', style: { color: 'var(--bad)' } });
    const d = dialog('Підключення до мережі', h('div', null,
      ssid ? null : h('label', { class: 'fld' }, h('span', null, 'Назва мережі'), ss),
      ssid ? h('p', { style: { marginTop: 0 } }, h('b', null, ssid)) : null,
      enc ? h('label', { class: 'fld' }, h('span', null, 'Пароль'), pw, show) : h('p', { class: 'note' }, 'Відкрита мережа — пароль не потрібен.'),
      h('p', { class: 'note' }, 'Радіо підключиться просто зараз, без перезавантаження, і скаже результат. Вдалося — мережа стане першою в списку; ні — радіо повернеться до мережі, де було. Якщо ця сторінка відкрита через ту саму мережу, зв\'язок на кілька секунд зникне.'), err), [
      btn('Скасувати', null, () => d.close(), 'ghost'),
      btn('Підключитися', 'wifi', async () => {
        const s = (ssid || ss.value).trim(), p = enc ? pw.value : '';
        if (!s) { err.textContent = 'Вкажіть назву мережі.'; return; }
        if (enc && p.length && p.length < 8) { err.textContent = 'Пароль Wi-Fi має бути не коротшим за 8 знаків.'; return; }
        const b = new URLSearchParams({ ssid: s, pass: p });
        try { await api('/api/wifi/join', { method: 'POST', body: b }); }
        catch (e) { if (!/помилка 4\d\d/.test(e.message) && !/задовга|немає назви/.test(e.message)) { /* зв'язок урвався — радіо вже перемикається */ } else { err.textContent = e.message; return; } }
        d.close();
        watch(s, enc);
      }, 'acc')]);
  }
  /*  Хід підключення: радіо відповідає в /api/wifi (join). Поки воно
      перемикається, сторінка може на кілька секунд втратити зв'язок.  */
  function watch(s, enc) {
    const box = h('section', { class: 'card' });
    page.prepend(box);
    const t0 = Date.now();
    let lost = 0;
    const show = (title, text, kind, extra) => {
      box.textContent = '';
      box.append(h('h2', null, title), h('p', { style: { color: kind === 'bad' ? 'var(--bad)' : kind === 'ok' ? 'var(--ok, #5ccf6a)' : '' } }, text), extra || null);
    };
    show('Підключаюся', `Радіо підключається до «${s}»…`);
    const tick = async () => {
      let j = null;
      try { j = (await api('/api/wifi')).join; lost = 0; } catch (e) { lost++; }
      const secs = (Date.now() - t0) / 1000;
      if (j && j.s === s && j.st !== 1 && j.ago >= 0 && j.ago <= secs + 5) {
        if (j.st === 2) {
          const here = location.hostname === j.ip;
          show('Готово', `Радіо в мережі «${s}», адреса ${j.ip}.`, 'ok',
            here ? null : h('p', null, h('a', { href: `http://${j.ip}/` }, `Відкрити сторінку радіо: http://${j.ip}/`)));
          load();
        } else {
          const why = j.st === 3 ? 'невірний пароль' : j.st === 4 ? 'мережі не видно (радіо бачить лише 2,4 ГГц)' : 'підключитися не вдалося';
          show('Не вийшло', `«${s}»: ${why}. Радіо повертається до мережі, де було.`, 'bad',
            h('div', { class: 'bar' }, btn('Спробувати ще', 'wifi', () => { box.remove(); join(s, enc); }, 'sm')));
          load();
        }
        return;
      }
      if (lost >= 4) show('Підключаюся', `Зв'язку зі сторінкою поки немає: радіо перемикається на «${s}». Якщо воно лишиться там, його нова адреса — внизу екрана радіо.`);
      if (secs < 60) setTimeout(tick, 1500);
      else show('Невідомо', 'Радіо так і не відповіло. Подивіться на екран радіо: там видно мережу й адресу.', 'bad');
    };
    setTimeout(tick, 1500);
  }
  load();
  if (!data || !data.scan.length) doScan();
  live(() => {});
};

/* ---------------------------------------------------------------- час і погода */
VIEWS.time = page => {
  const tz = h('div'), we = h('div'), now = h('p', { class: 'note' });
  page.append(card('Час', now, tz), card('Погода', we));
  send('gettimezone=1'); send('getweather=1');
  let ts = '', wsig = '';
  live(() => {
    if (S) now.textContent = S.time ? `На радіо зараз ${S.time}, ${S.date}.` : 'Радіо ще не отримало точний час.';
    if ('tzh' in C && S && ts !== [C.tzh, C.tzm, C.sntp1, C.sntp2, C.timeint, S.dst].join('|')) {
      ts = [C.tzh, C.tzm, C.sntp1, C.sntp2, C.timeint, S.dst].join('|');
      tz.textContent = '';
      const hs = selectEl(Array.from({ length: 27 }, (_, i) => [i - 12, (i - 12 >= 0 ? '+' : '') + (i - 12)]), C.tzh, () => {});
      const ms = selectEl([[0, ':00'], [15, ':15'], [30, ':30'], [45, ':45']], C.tzm, () => {});
      const s1 = h('input', { type: 'text', value: C.sntp1, maxlength: 34 }), s2 = h('input', { type: 'text', value: C.sntp2, maxlength: 34 });
      const iv = h('input', { type: 'number', min: 15, max: 1440, value: C.timeint, style: { width: '100px' } });
      const dstOn = S && S.dst === 1;
      const dst = sw(dstOn, v => { setx({ dst: v ? 1 : 0 }).then(() => toast(v ? 'Літній час радіо ставитиме саме' : 'Літній час вимкнено: пояс — як вибрано')); });
      tz.append(
        h('div', { class: 'fld' }, h('span', null, 'Часовий пояс (UTC)'), h('div', { class: 'bar' }, h('div', { style: { width: '100px' } }, hs), h('div', { style: { width: '100px' } }, ms), h('span', { class: 'note', style: { margin: 0 } }, dstOn ? 'пояс зимовий — Україна: +2' : 'Україна: +2 взимку, +3 влітку'))),
        row('Літній час — сам', 'остання неділя березня й жовтня, як у Європі: пояс вибирайте зимовий', dst),
        h('div', { class: 'grid2' }, h('label', { class: 'fld' }, h('span', null, 'Сервер часу'), s1), h('label', { class: 'fld' }, h('span', null, 'Запасний сервер'), s2)),
        h('label', { class: 'fld inl' }, h('span', null, 'Звіряти час кожні, хв'), iv),
        btn('Застосувати', 'check', () => { send('tzh=' + hs.value); send('tzm=' + ms.value); send('sntp2=' + s2.value.trim()); send('sntp1=' + s1.value.trim()); send('timeint=' + iv.value); toast('Збережено'); }, 'acc'));
    }
    if ('wen' in C && wsig !== [C.wen, C.wlat, C.wlon, C.wkey, C.wint].join('|')) {
      wsig = [C.wen, C.wlat, C.wlon, C.wkey, C.wint].join('|');
      we.textContent = '';
      const lat = h('input', { type: 'text', value: C.wlat, inputmode: 'decimal' }), lon = h('input', { type: 'text', value: C.wlon, inputmode: 'decimal' });
      const key = h('input', { type: 'text', value: C.wkey, spellcheck: 'false', placeholder: 'ключ OpenWeatherMap' });
      const iv = h('input', { type: 'number', min: 15, max: 1440, value: C.wint, style: { width: '100px' } });
      we.append(row('Показувати погоду', 'на екрані плеєра', sw(+C.wen, v => send('showweather=' + (v ? 1 : 0)))),
        h('div', { class: 'grid2', style: { marginTop: '10px' } }, h('label', { class: 'fld' }, h('span', null, 'Широта'), lat), h('label', { class: 'fld' }, h('span', null, 'Довгота'), lon)),
        h('label', { class: 'fld' }, h('span', null, 'Ключ API'), key),
        h('label', { class: 'fld inl' }, h('span', null, 'Оновлювати кожні, хв'), iv),
        h('div', { class: 'bar' }, btn('Застосувати', 'check', () => { send('lat=' + lat.value.trim()); send('lon=' + lon.value.trim()); send('wint=' + iv.value); if (key.value.trim()) send('key=' + key.value.trim()); toast('Збережено'); }, 'acc'),
          btn('Моє місце', 'globe', () => { if (!navigator.geolocation) return toast('Браузер не знає місця', true); navigator.geolocation.getCurrentPosition(p => { lat.value = p.coords.latitude.toFixed(4); lon.value = p.coords.longitude.toFixed(4); }, () => toast('Місце не визначилось (потрібен дозвіл браузера)', true)); }, 'ghost')),
        h('p', { class: 'note' }, 'Безкоштовний ключ дає openweathermap.org після реєстрації.'));
    }
  });
};

/* ---------------------------------------------------------------- система */
VIEWS.system = page => {
  const box = h('div');
  page.append(card('Поведінка', box),
    card('Живлення',
      row('Перезавантажити', 'звук перерветься приблизно на 10 секунд; налаштування й списки лишаються',
        btn('Перезавантажити', 'refresh', async () => { if (await confirmBox('Перезавантажити радіо?', 'Звук перерветься приблизно на 10 секунд.', 'Перезавантажити')) { setx({ power: 'reboot' }); toast('Перезавантажується…'); setTimeout(() => location.reload(), 12000); } }, 'sm')),
      row('Вимкнути', 'глибокий сон: радіо майже не бере заряд; увімкнути — дотиком до екрана радіо',
        btn('Вимкнути', 'x', async () => { if (await confirmBox('Вимкнути радіо?', 'Радіо засне й зникне з мережі. Увімкнути його звідси вже не вийде — лише дотиком до екрана.', 'Вимкнути', true)) { await setx({ power: 'off' }); toast('Радіо вимикається. Щоб увімкнути — торкніться його екрана.'); } }, 'sm bad'))));
  send('getsystem=1'); send('getcontrols=1');
  let s = '';
  live(() => {
    if (!('sst' in C)) return;
    const sig = ['sst', 'aif', 'vu', 'softr', 'mdns', 'telnet', 'watchdog'].map(k => C[k]).join('|');
    if (sig === s) return; s = sig;
    box.textContent = '';
    const sa = h('input', { type: 'number', min: 0, max: 30, value: C.softr, style: { width: '90px' } }); sa.onchange = () => send('softap=' + sa.value);
    const md = h('input', { type: 'text', value: C.mdns, maxlength: 24, style: { width: '180px' } });
    box.append(
      row('Грати після ввімкнення', 'продовжити станцію, якщо радіо грало, коли його вимкнули', sw(+C.sst, v => send('smartstart=' + (v ? 1 : 0)))),
      row('Подробиці потоку в журналі', 'для діагностики через USB', sw(+C.aif, v => send('audioinfo=' + (v ? 1 : 0)))),
      row('Бездротова колонка (DLNA)', 'радіо видно в мережі як колонку: телефон чи комп\'ютер надсилає йому доріжку — BubbleUPnP, Hi-Fi Cast, VLC, «Передати на пристрій» у Windows',
        sw(S && S.dlna, v => setx({ dlna: v ? 1 : 0 }))),
      row('Колонка AirPlay', 'iPhone, iPad і Mac грають на радіо будь-який свій звук: «Звук» у Пункті керування чи кнопка AirPlay у програмі' +
        (S && S.airplay && S.ap && !S.ap.key ? ' — радіо ще отримує ключ AirPort' + (S.ap.err ? ' (' + S.ap.err + ')' : '') : ''),
        sw(S && S.airplay, v => setx({ airplay: v ? 1 : 0 }))),
      row('Точка доступу, якщо мережі немає', 'через стільки хвилин (0 — одразу)', sa),
      row('Ім\'я в мережі', `відкривається як http://${C.mdns || 'potuzhne'}.local`, h('div', { class: 'bar' }, md, btn('Змінити', null, () => { send('mdnsname=' + md.value.trim()); send('rebootmdns=1'); toast('Радіо перезавантажується з новим ім\'ям'); }, 'sm'))),
      row('Telnet', 'керування через telnet у локальній мережі', sw(+C.telnet, v => send('telnet=' + (v ? 1 : 0)))),
      row('Сторож', 'перезапуск, якщо звук завис', sw(+C.watchdog, v => send('watchdog=' + (v ? 1 : 0)))));
  });
};

/* ---------------------------------------------------------------- розробник */
const DACS_28 = [
  { n: 'ES8311', k: 'вбудований кодек і підсилювач', d: ['Кодек ES8311 і підсилювач SC8002B на динамік плати.', 'Нічого під\'єднувати не треба. (ES8388 на цій платі немає.)'], p: [] },
  { n: 'PCM5102A', k: 'стерео ЦАП, лінійний вихід', d: ['Стерео ЦАП із лінійним виходом 3,5 мм — для підсилювача чи колонок.', 'SCK модуля — на GND, XSMT — на 3,3 В, інакше звуку не буде.'],
    p: [['5V UART', 'VIN'], ['GND UART', 'GND'], ['IO14', 'BCK'], ['IO21', 'LCK'], ['IO2', 'DIN'], ['GND I2C', 'SCK'], ['3V3 I2C', 'XSMT']] },
  { n: 'UDA1334A', k: 'стерео ЦАП, навушники й лінія', d: ['Стерео ЦАП із виходом на навушники й лінію.', 'Живлення від 3 до 5 В.'],
    p: [['5V UART', 'VIN'], ['GND UART', 'GND'], ['IO14', 'BCLK'], ['IO21', 'WSEL'], ['IO2', 'DIN']] },
  { n: 'MAX98357A', k: 'моно підсилювач 3 Вт', d: ['Моно підсилювач класу D на динамік 4–8 Ом, до 3 Вт.', 'Виводи GAIN і SD не під\'єднувати.'],
    p: [['5V UART', 'VIN'], ['GND UART', 'GND'], ['IO14', 'BCLK'], ['IO21', 'LRC'], ['IO2', 'DIN']] },
  { n: 'VS1053B', k: 'окремий декодер, інша прошивка', d: ['Апаратний декодер MP3/AAC/FLAC зі своїм виходом.', 'Під\'єднується без пайки — лише роз\'єми на звороті плати. Потрібна окрема прошивка: з цією він не заграє.'],
    p: [['5V UART', '5V'], ['GND UART', 'GND'], ['IO14', 'SCK'], ['IO21', 'MOSI'], ['IO3', 'MISO'], ['IO2', 'XCS'], ['TXD0', 'XDCS'], ['RXD0', 'DREQ'], ['3V3 I2C', 'XRST']], alt: true },
];
/*  ES3C35P (3,5″): ЦАП — на штатних роз'ємах 4P 1,25 мм на звороті плати:
    Expand (P3) 1 3V3, 2 GND, 3 IO46, 4 IO45; UART (P2) 1 +5 В, 2 GND, 3 TXD0 (IO43), 4 RXD0 (IO44).
    VS1053B під'єднати нікуди — вільних сигналів на роз'ємах лише чотири.  */
const DACS_35 = [
  { n: 'ES8311', k: 'вбудований кодек і підсилювач', d: ['Кодек ES8311 і підсилювач SC8002B на динамік плати.', 'Нічого під\'єднувати не треба.'], p: [] },
  { n: 'PCM5100/5102A', k: 'стерео ЦАП, лінійний вихід', d: ['Стерео ЦАП із лінійним виходом 3,5 мм — для підсилювача чи колонок (PCM5100 і PCM5102A під\'єднуються однаково).', 'SCK модуля — на GND, XSMT — на 3,3 В, FMT — на GND (I2S), інакше звуку не буде.'],
    p: [['5V UART', 'VIN'], ['GND UART', 'GND'], ['IO45', 'BCK'], ['IO46', 'LCK'], ['TXD0', 'DIN'], ['GND Expand', 'SCK'], ['3V3 Expand', 'XSMT']] },
  { n: 'UDA1334A', k: 'стерео ЦАП, навушники й лінія', d: ['Стерео ЦАП із виходом на навушники й лінію.', 'Живлення від 3 до 5 В.'],
    p: [['5V UART', 'VIN'], ['GND UART', 'GND'], ['IO45', 'BCLK'], ['IO46', 'WSEL'], ['TXD0', 'DIN']] },
  { n: 'MAX98357A', k: 'моно підсилювач 3 Вт', d: ['Моно підсилювач класу D на динамік 4–8 Ом, до 3 Вт.', 'Виводи GAIN і SD не під\'єднувати.'],
    p: [['5V UART', 'VIN'], ['GND UART', 'GND'], ['IO45', 'BCLK'], ['IO46', 'LRC'], ['TXD0', 'DIN']] },
];
const is35 = () => !!(S && S.board === 'ES3C35P');
const dacs = () => is35() ? DACS_35 : DACS_28;
const dacOf = i => dacs()[i] || dacs()[0];
VIEWS.dev = page => {
  const tg = h('div'), dac = h('div'), lg = h('div');
  const snd = sfxSection();
  page.append(h('p', { class: 'lead' }, 'Для тих, хто перебудовує радіо: що показувати на екрані, заставка, звуки подій і куди йде звук.'),
    card('На екрані', tg), ...snd.els, card('Аудіовихід', dac), card('Логотипи станцій', lg));
  let sel = null, s1 = '', s2 = '';
  live(() => {
    if (!S) return;
    snd.upd();
    const d = S.dev;
    const sig1 = [d.noBat, d.noIp, d.noSd].join('|');
    if (sig1 !== s1) {
      s1 = sig1; tg.textContent = '';
      tg.append(row('Батарея', 'значок заряду й попередження про низький заряд', sw(!d.noBat, v => setx({ noBat: v ? 0 : 1 }))),
        row('IP-адреса', 'на головному екрані — щоб знати, куди заходити з браузера', sw(!d.noIp, v => setx({ noIp: v ? 0 : 1 }))),
        row('Функції картки пам\'яті', 'плеєр картки, запис ефіру, кнопка джерела в шапці', sw(!d.noSd, v => setx({ noSd: v ? 0 : 1 }))));
    }
    if (sel === null) sel = d.dac;
    const sig2 = sel + '|' + d.dac;
    if (sig2 !== s2) {
      s2 = sig2; dac.textContent = '';
      const cards = h('div', { class: 'dacs' });
      dacs().forEach((x, i) => cards.append(h('button', { type: 'button', class: 'dac' + (i === sel ? ' sel' : '') + (i === d.dac ? ' cur' : ''), onclick: () => { sel = i; s2 = ''; pageLive('x'); } },
        h('b', null, x.n), h('small', null, i === d.dac ? 'зараз звук іде сюди' : x.k))));
      const x = dacOf(sel);
      const det = h('div', { style: { marginTop: '14px' } }, h('p', { style: { margin: '0 0 4px' } }, x.d[0]), h('p', { class: 'note', style: { marginTop: 0 } }, x.d[1]));
      if (x.p.length) {
        det.append(h('table', { class: 'wire' }, h('tbody', null, h('tr', null, h('td', { class: 'gd' }, 'Плата (роз\'єм)'), h('td'), h('td', { class: 'gd' }, x.n)),
          x.p.map(([a, b]) => h('tr', null, h('td', { class: /^(5V|3V3)/.test(a) ? 'pw' : /^GND/.test(a) ? 'gd' : 'io' }, a), h('td', null, '→'), h('td', null, b))))));
        const used = new Set(x.p.map(p => p[0].replace(/ .*/, '')));
        const HD = is35()
          ? [['Роз\'єм розширення (Expand, P3)', ['3V3', 'GND', 'IO46', 'IO45']], ['Роз\'єм UART (P2)', ['5V', 'GND', 'TXD0', 'RXD0']], ['Роз\'єм I2C (P4)', ['3V3', 'GND', 'SCL', 'SDA']]]
          : [['Роз\'єм розширення (Expand)', ['IO2', 'IO3', 'IO14', 'IO21']], ['Роз\'єм UART', ['TXD0', 'RXD0', 'GND', '5V']], ['Роз\'єм I2C', ['3.3V', 'GND', 'SCL', 'SDA']]];
        const u = is35()
          ? (hn, pin) => { const tag = hn.includes('UART') ? 'UART' : hn.includes('I2C') ? 'I2C' : 'Expand'; return x.p.some(p => p[0] === pin || p[0] === pin + ' ' + tag); }
          : (hn, pin) => (pin === '3.3V' && x.p.some(p => p[0] === '3V3 I2C')) || (pin === 'GND' && x.p.some(p => p[0] === (hn.includes('UART') ? 'GND UART' : 'GND I2C'))) || (pin === '5V' && x.p.some(p => p[0] === '5V UART')) || used.has(pin);
        det.append(h('div', { class: 'hdrs' }, HD.map(([n, pins]) => h('div', { class: 'hdr' }, h('b', null, n), h('ol', null, pins.map(p => h('li', { class: u(n, p) ? 'u' : '' }, p)))))),
          h('p', { class: 'note' }, is35() ? 'Усі три роз\'єми — 4P 1,25 мм на звороті плати, паяти не треба; виводи пронумеровано від 1 по схемі виробника. Жовтим позначено виводи, які використовує цей модуль.' : 'Усі три роз\'єми — на звороті плати, паяти не треба. Жовтим позначено виводи, які використовує цей модуль. Дроти — 1,25 мм (Expand) і 2,54 мм.'));
      }
      det.append(h('div', { class: 'bar', style: { marginTop: '12px' } },
        sel === d.dac ? h('span', { class: 'pill ok' }, 'Звук іде сюди') :
        x.alt ? h('span', { class: 'pill bad' }, 'Потрібна окрема прошивка') :
        btn('Увімкнути ' + x.n, 'check', async () => { if (sel !== 0 && !await confirmBox('Перемкнути звук?', `Звук піде на ${x.n}. Вбудований динамік замовкне. Повернути можна тут або в меню радіо «розробник».`, 'Перемкнути')) return; await setx({ dac: sel }); setTimeout(pollState, 500); }, 'acc')));
      dac.append(cards, det);
    }
    if (!lg.dataset.ok) {
      lg.dataset.ok = 1;
      lg.append(h('div', { class: 'bar' }, h('span', { style: { flex: 1 } }, 'Радіо саме шукає логотипи в каталозі radio-browser.info, коли станція заграє. Не знайдені позначаються, щоб не питати щоразу.'),
        btn('Шукати знову', 'refresh', async () => { await setx({ logoForget: 1 }); toast('Позначки скинуто — логотипи знайдуться, коли станції заграють'); }, 'sm')),
        h('p', { class: 'note' }, 'Свій логотип для станції можна поставити в розділі «Станції» → олівець → «Свій логотип».'));
    }
  });
};

/* ---------------------------------------------------------------- оновлення */
function xhrUpload(url, fd, bar, note) {
  return new Promise((res, rej) => {
    const x = new XMLHttpRequest();
    x.open('POST', url);
    x.upload.onprogress = e => { if (e.lengthComputable) { bar.style.width = (e.loaded / e.total * 100) + '%'; note.textContent = `Надіслано ${size(e.loaded)} з ${size(e.total)}`; } };
    x.onload = () => (x.status >= 200 && x.status < 400) ? res(x.responseText) : rej(new Error(x.responseText || 'помилка ' + x.status));
    x.onerror = () => rej(new Error('зв\'язок перервався'));
    x.send(fd);
  });
}
function waitBack(note, was) {
  let n = 0;
  const t = setInterval(async () => {
    n++; note.textContent = `Радіо перезавантажується… ${n * 2} с`;
    try {
      const r = await fetch('/api/state', { cache: 'no-store' });
      if (r.ok) {
        const s = await r.json(); clearInterval(t);
        note.textContent = s.build && s.build !== was ? `Готово: тепер ПОТУЖНЕ РАДІО ${s.v}, зібрано ${s.build}.` : 'Радіо знову на зв\'язку.';
        setTimeout(() => location.reload(), 2500);
      }
    } catch (e) {}
    if (n > 60) { clearInterval(t); note.textContent = 'Радіо довго не відповідає. Оновіть сторінку вручну.'; }
  }, 2000);
}
VIEWS.update = page => {
  const now = h('dl', { class: 'kv' });
  const drawNow = () => { now.textContent = ''; now.append(h('dt', null, 'Версія'), h('dd', null, (S && S.v) || VER || '—'), h('dt', null, 'Зібрано'), h('dd', null, (S && S.build) || BUILD || '—')); };
  drawNow();

  const bar1 = h('i'), note1 = h('p', { class: 'note' }), err1 = h('p', { class: 'note', style: { color: 'var(--bad)' } });
  const what = h('div');
  const fwp = filePick('.bin', false, 'Вибрати файл прошивки'), fw = fwp.input;
  let fwInfo = null;
  const go1 = btn('Оновити радіо', 'upload', async () => {
    const f = fw.files[0];
    if (!f || !fwInfo) { err1.textContent = 'Спершу виберіть файл PotuzhneRadio-ES3C28P-update.bin.'; return; }
    const same = S && S.build === fwInfo.build;
    if (!await confirmBox('Оновити прошивку?', `ПОТУЖНЕ РАДІО ${fwInfo.ver}, зібрано ${fwInfo.build}.${same ? ' Це та сама збірка, що вже стоїть.' : ''} Звук зупиниться; не вимикайте радіо близько хвилини, доки йде оновлення.`, 'Оновити')) return;
    err1.textContent = ''; go1.disabled = true; fwp.querySelector('button').disabled = true;
    const was = S && S.build;
    try {
      const fd = new FormData(); fd.append('updatetarget', 'fw'); fd.append('update', f, f.name);
      const r = await xhrUpload('/update', fd, bar1, note1);
      if (!/^OK/.test(r)) throw new Error(r.replace(/<[^>]+>/g, ' '));
      note1.textContent = 'Прошивку записано. Радіо перезавантажується…';
      waitBack(note1, was);
    } catch (e) { err1.textContent = 'Не вдалося: ' + e.message; go1.disabled = false; fwp.querySelector('button').disabled = false; }
  }, 'acc');
  go1.disabled = true;
  fw.addEventListener('change', async () => {
    fwInfo = null; go1.disabled = true; err1.textContent = ''; what.textContent = '';
    const f = fw.files[0]; if (!f) return;
    what.append(h('p', { class: 'note' }, 'Перевіряю файл…'));
    const r = await readFirmware(f);
    what.textContent = '';
    if (r.err) { err1.textContent = r.err; return; }
    fwInfo = r;
    const a = parseBuild(r.build), b = parseBuild(S && S.build);
    const cmp = !b || !a ? '' : a > b ? 'новіша за ту, що на радіо' : a < b ? 'старіша за ту, що на радіо — радіо повернеться до неї' : 'та сама, що вже на радіо';
    what.append(h('dl', { class: 'kv', style: { margin: '4px 0 12px' } },
      h('dt', null, 'У файлі'), h('dd', null, `ПОТУЖНЕ РАДІО ${r.ver}`),
      h('dt', null, 'Зібрано'), h('dd', null, r.build, cmp ? h('span', { class: 'pill ' + (a > b ? 'ok' : a < b ? 'bad' : ''), style: { marginLeft: '8px' } }, cmp) : null),
      h('dt', null, 'Розмір'), h('dd', null, size(f.size))));
    go1.disabled = false;
  });

  const bar2 = h('i'), note2 = h('p', { class: 'note' }), err2 = h('p', { class: 'note', style: { color: 'var(--bad)' } });
  const wfp = filePick('.gz,.js,.css', true, 'Вибрати файли сторінки'), wf = wfp.input;
  const go2 = btn('Оновити сторінку', 'upload', async () => {
    const fs = [...wf.files];
    if (!fs.length) { err2.textContent = 'Виберіть app.js.gz і app.css.gz.'; return; }
    const bad = fs.filter(f => !WEB_OK.test(f.name));
    if (bad.length) { err2.textContent = 'Це не файли сторінки: ' + bad.map(f => f.name).join(', ') + '. Потрібні app.js.gz і app.css.gz.'; return; }
    err2.textContent = ''; go2.disabled = true;
    try {
      const fd = new FormData(); fs.forEach(f => fd.append('www', f, f.name));
      await xhrUpload('/webboard', fd, bar2, note2);
      note2.textContent = 'Готово. Оновлюю сторінку…'; setTimeout(() => location.reload(), 1500);
    } catch (e) { err2.textContent = 'Не вдалося: ' + e.message; go2.disabled = false; }
  }, 'acc');

  /*  резервна копія: обране живе в пам'яті налаштувань, тож віддаємо його файлом тут  */
  const favFile = h('input', { type: 'file', accept: '.json', hidden: true });
  favFile.onchange = async () => {
    const f = favFile.files[0]; if (!f) return;
    let arr; try { arr = JSON.parse(await f.text()); } catch (e) { toast('Це не файл обраного', true); return; }
    if (!Array.isArray(arr)) { toast('Це не файл обраного', true); return; }
    for (let i = 0; i < 6; i++) { const x = arr[i] || {}; if (x.n && /^https?:\/\//.test(x.u || '')) await setx({ favPut: `${i}\t${x.n}\t${x.u}` }); }
    toast('Обране відновлено'); pollState();
  };
  const wifiFile = h('input', { type: 'file', accept: '.csv,.txt', hidden: true });
  wifiFile.onchange = async () => {
    const f = wifiFile.files[0]; if (!f) return;
    const lines = (await f.text()).split(/\r?\n/).filter(l => /^[^\t]+\t.*$/.test(l));
    if (!lines.length) { toast('У файлі немає жодної мережі (формат: назва, табуляція, пароль)', true); return; }
    if (!await confirmBox('Відновити мережі?', `${lines.length} ${plural(lines.length, 'мережа', 'мережі', 'мереж')} із файлу замінять збережені. Нові набудуть сили після перезавантаження радіо.`, 'Відновити')) return;
    const fd = new FormData(); fd.append('data', new Blob([lines.slice(0, 5).join('\n') + '\n']), 'wifi.csv');
    try { await api('/webboard', { method: 'POST', body: fd }); toast('Мережі записано. Перезавантажте радіо в розділі «Система».'); } catch (e) { toast('Не вдалося: ' + e.message, true); }
  };

  page.append(
    otaCard(),
    card('Зараз на радіо', now),
    card('Прошивка з файлу',
      h('ol', { style: { margin: '0 0 12px', paddingLeft: '20px', lineHeight: 1.6 } },
        h('li', null, 'Зберіть прошивку: ', h('code', null, 'firmware/rebuild.sh'), ' (або візьміть готову).'),
        h('li', null, 'Виберіть файл ', h('b', null, 'firmware/PotuzhneRadio-ES3C28P-update.bin'), ' — сторінка покаже його версію й дату збірки.'),
        h('li', null, 'Натисніть «Оновити радіо». Станції, мережі, обране й налаштування лишаються.')),
      fwp, what, h('div', { class: 'prog' }, bar1), h('div', { class: 'bar' }, go1), err1, note1),
    card('Сторінка', h('p', { style: { marginTop: 0 } }, 'Файли ', h('b', null, 'app.js.gz'), ' і ', h('b', null, 'app.css.gz'), ' з теки ', h('b', null, 'firmware/web/'), '. Прошивка й сторінка оновлюються окремо; на час надсилання звук зупиниться.'),
      wfp, h('div', { class: 'prog' }, bar2), h('div', { class: 'bar' }, go2), err2, note2),
    card('Резервна копія',
      row('Список станцій', 'playlist.csv — повернути можна в «Станції → Імпорт» із переглядом', h('a', { class: 'btn sm', href: '/api/stations', download: 'playlist.csv' }, ic('download'), 'Зберегти')),
      row('Обране', 'шість кнопок швидкого вибору', h('div', { class: 'bar' }, btn('Зберегти', 'download', () => download('obrane.json', JSON.stringify((S && S.fav) || [], null, 1), 'application/json'), 'sm'), btn('Відновити', 'upload', () => favFile.click(), 'sm ghost'), favFile)),
      row('Мережі Wi-Fi', 'wifi.csv — з паролями відкритим текстом, зберігайте надійно', h('div', { class: 'bar' }, h('a', { class: 'btn sm', href: '/data/wifi.csv', download: 'wifi.csv' }, ic('download'), 'Зберегти'), btn('Відновити', 'upload', () => wifiFile.click(), 'sm ghost'), wifiFile))),
    card('Якщо сторінка не відкривається', h('p', { style: { margin: 0 } }, 'Під\'єднайте радіо кабелем USB до Mac і в теці ', h('b', null, 'firmware'), ' виконайте ', h('code', null, './flash.sh --app-only'), ' — оновиться лише програма, списки лишаться. ', h('code', null, './flash.sh'), ' без параметрів пише повний образ і стирає станції та мережі.')));
  live(() => { drawNow(); otaCard.draw && otaCard.draw(); });
};

/* ---------------------------------------------------------------- оновлення з GitHub */
/*  Стан приходить у /api/state (S.ota): st — 0 нічого, 1 перевіряю, 2 остання, 3 є новіша, 4 помилка,
    5 зупиняю звук, 6 сторінка, 7 прошивка, 8 перевірка, 9 готово.  */
let otaDismiss = '';
try { otaDismiss = sessionStorage.getItem('otaDismiss') || ''; } catch (e) {}
function otaInstallAsk(tag) {
  return confirmBox('Оновити радіо?', `Буде встановлено ПОТУЖНЕ РАДІО ${tag} з GitHub. Звук зупиниться, радіо саме завантажить прошивку й перезавантажиться — близько двох хвилин. Станції, мережі й налаштування лишаються.`, 'Оновити');
}
function otaBar() {
  const bar = document.getElementById('otabar');
  if (!bar || !S || !S.ota) return;
  const o = S.ota;
  bar.textContent = '';
  if (o.st >= 5) {
    const pct = o.st === 7 ? o.pct : o.st >= 8 ? 100 : 0;
    const mb = o.st === 7 && o.size ? ` · ${(o.got / 1048576).toFixed(1)} з ${(o.size / 1048576).toFixed(1)} МБ · ${Math.round(o.bps / 1024)} КБ/с` : '';
    bar.append(h('div', { class: 'otatx' }, h('b', null, `Оновлення до ${o.tag}: `), `${o.step}${o.st === 7 ? ' ' + pct + '%' : ''}${mb}`),
      h('div', { class: 'prog' }, h('i', { style: { width: pct + '%' } })));
    bar.hidden = false;
    return;
  }
  if (o.avail && o.tag !== otaDismiss) {
    bar.append(h('div', { class: 'otatx' }, h('b', null, `Вийшла нова версія ${o.tag}`), ` (у радіо ${S.v})`),
      h('div', { class: 'bar' },
        btn('Пізніше', null, () => { otaDismiss = o.tag; try { sessionStorage.setItem('otaDismiss', o.tag); } catch (e) {} otaBar(); }, 'sm ghost'),
        btn('Оновити', 'upload', async () => { if (await otaInstallAsk(o.tag)) { await setx({ otaInstall: 1 }); pollState(); } }, 'sm acc')));
    bar.hidden = false;
    return;
  }
  bar.hidden = true;
}

function otaCard() {
  const kv = h('dl', { class: 'kv' });
  const notes = h('div', { class: 'note', style: { whiteSpace: 'pre-wrap', margin: '8px 0' } });
  const stat = h('p', { class: 'note' });
  const progI = h('i'), prog = h('div', { class: 'prog', hidden: true }, progI);
  let notesTag = '';
  const chk = btn('Перевірити зараз', 'refresh', async () => { await setx({ otaCheck: 0 }); setTimeout(pollState, 1500); }, 'sm');
  const ins = btn('Оновити', 'upload', async () => { const o = S.ota; if (await otaInstallAsk(o.tag)) { await setx({ otaInstall: 1 }); pollState(); } }, 'acc');
  const draw = async () => {
    const o = (S && S.ota) || {};
    kv.textContent = '';
    kv.append(h('dt', null, 'У радіо'), h('dd', null, (S && S.v) || '—'),
      h('dt', null, 'На GitHub'), h('dd', null, o.tag || (o.st === 1 ? 'перевіряю…' : 'ще не перевіряли'), o.avail ? h('span', { class: 'pill ok', style: { marginLeft: '8px' } }, 'новіша') : null));
    const txt = { 1: 'Звертаюсь до GitHub…', 2: 'У радіо остання версія.', 4: 'Не вийшло: ' + (o.err || '') }[o.st];
    stat.textContent = o.st >= 5 ? `${o.step}${o.st === 7 ? ' — ' + o.pct + '%' : ''}` : (txt || (o.avail ? '' : 'Радіо саме перевіряє GitHub двічі на добу.'));
    prog.hidden = !(o.st >= 5); progI.style.width = (o.st === 7 ? o.pct : o.st >= 8 ? 100 : 0) + '%';
    chk.disabled = o.st === 1 || o.st >= 5;
    ins.hidden = !o.avail || o.st >= 5;
    if (o.avail) ins.lastChild.textContent = 'Оновити до ' + o.tag;
    if (o.tag && o.tag !== notesTag) {
      notesTag = o.tag;
      try { const r = await api('/api/ota'); notes.textContent = r.notes || ''; } catch (e) {}
    }
    notes.hidden = !o.avail;
  };
  otaCard.draw = draw;
  draw();
  return card('З GitHub', kv, notes, stat, prog, h('div', { class: 'bar' }, chk, ins),
    h('p', { class: 'note', style: { margin: '8px 0 0' } }, 'Нові версії виходять на github.com/roman885-85/potuzhne-radio. Радіо само завантажує прошивку й сторінку, станції та налаштування лишаються.'));
}

/* ---------------------------------------------------------------- про пристрій */
VIEWS.about = page => {
  const kv = h('dl', { class: 'kv' });
  /*  плата — з /api/state (board): ES3C28P або ES3C35P  */
  const BOARD = {
    ES3C28P: 'Плата ES3C28P: ESP32-S3, екран 2,8″ ILI9341 із сенсором, кодек ES8311, картка пам\'яті, 16 МБ флеш, 8 МБ PSRAM.',
    ES3C35P: 'Плата ES3C35P: ESP32-S3, екран 3,5″ ST77922 (QSPI) із вбудованим сенсором, кодек ES8311, картка пам\'яті, 16 МБ флеш, 8 МБ PSRAM.',
  };
  const boardNote = h('p', { class: 'note', style: { margin: 0 } }, BOARD[(S && S.board) || 'ES3C28P'] || ('Плата ' + ((S && S.board) || '')));
  page.append(card('ПОТУЖНЕ РАДІО', kv), card(null, boardNote));
  live(() => {
    if (!S) return;
    if (S.board && BOARD[S.board]) boardNote.textContent = BOARD[S.board];
    const up = S.up, d = Math.floor(up / 86400);
    const rows = [
      ['Версія', `${S.v}, збірка ${S.build || '—'}`],
      ['Мережа', S.ssid || 'точка доступу'], ['Адреса', S.ip], ['Сигнал', S.rssi ? S.rssi + ' дБм' : '—'],
      ['Час на радіо', S.time ? `${S.time}, ${S.date}` : 'ще невідомий'],
      ['Працює', (d ? d + ' д ' : '') + mmss(up % 86400)],
      ['Потік', W.playing && W.bitrate ? `${W.bitrate} кбіт/с${S.codec ? ', ' + S.codec : ''}` : '—'],
      ['Батарея', S.dev.noBat ? 'вимкнено' : S.bat.pct >= 0 ? `${S.bat.pct}% · ${(S.bat.mv / 1000).toFixed(2)} В${S.bat.chg ? ' · заряджається' : S.bat.full ? ' · заряджено' : S.bat.pwr ? ' · від USB' : ''}` : 'немає'],
      ['Пам\'ять', `${size(S.heap)} вільно, PSRAM ${size(S.psram)}`],
      ['Файлова система', `${size(S.fs.u)} з ${size(S.fs.t)}`],
      ['Аудіовихід', dacOf(S.dev.dac).n],
    ];
    /*  Сторож зависань (extras/yoHang): радіо перезапустилось саме — що саме стояло й коли.  */
    if (S.hang) rows.push(['Останнє зависання', `${S.hang}${S.hangAt ? ' · ' + new Date(S.hangAt * 1000).toLocaleString('uk-UA', { day: 'numeric', month: 'long', hour: '2-digit', minute: '2-digit' }) : ''} — радіо перезапустилось саме`]);
    kv.textContent = '';
    rows.forEach(([a, b]) => kv.append(h('dt', null, a), h('dd', null, b)));
  });
};

/* ---------------------------------------------------------------- звуки подій
   Файли живуть на радіо в розділі ресурсів. Свій звук сторінка готує сама:
   будь-який файл, який уміє браузер, → моно 22 кГц, до 5 с, вирівняна
   гучність → WAV. Радіо не мусить уміти MP3 чи OGG для таких дрібниць.  */
const SFX_MAX_SEC = 10, SFX_MAX_FILE = 20 * 1024 * 1024;   /* свій звук: до 10 с після перетворення; вихідний файл до 20 МБ */
const SFX_SUB = {
  start: 'коли радіо вмикається', click: 'клацання під пальцем; типово вимкнено', gesture: 'хлопки чи стук розпізнано',
  connect: 'радіо повернулось у мережу', error: 'зв\'язок із мережею зник', timer: 'таймер сну спрацював', alarm: 'на початку будильника, перед станцією',
  battery: 'щохвилини, поки батарея майже порожня (звучить завжди)',
};

async function sfxToWav(file, maxSec) {
  const Ctx = window.AudioContext || window.webkitAudioContext;
  const OCtx = window.OfflineAudioContext || window.webkitOfflineAudioContext;
  if (!Ctx || !OCtx) throw new Error('цей браузер не вміє перетворювати звук');
  const raw = await file.arrayBuffer();
  const ctx = new Ctx();
  let audio;
  try {
    audio = await new Promise((res, rej) => { const p = ctx.decodeAudioData(raw, res, rej); if (p && p.then) p.then(res, rej); });
  } catch (e) { throw new Error('файл не схожий на звук'); }
  finally { try { ctx.close(); } catch (e) {} }
  const RATE = 22050;
  const secs = Math.min(audio.duration, maxSec);
  const len = Math.max(1, Math.floor(secs * RATE));
  const off = new OCtx(1, len, RATE);
  const src = off.createBufferSource(); src.buffer = audio; src.connect(off.destination); src.start(0);
  const out = await off.startRendering();
  const d = out.getChannelData(0);
  let pk = 0; for (let i = 0; i < d.length; i++) { const a = Math.abs(d[i]); if (a > pk) pk = a; }
  const g = pk > 0 ? Math.min(0.708 / pk, 8) : 1;               /* пік −3 дБ, тихе підсилити не більше ніж у 8 разів */
  const fade = Math.min(len, Math.floor(RATE * 0.01));
  const ab = new ArrayBuffer(44 + len * 2), v = new DataView(ab);
  const str = (o, t) => { for (let i = 0; i < t.length; i++) v.setUint8(o + i, t.charCodeAt(i)); };
  str(0, 'RIFF'); v.setUint32(4, 36 + len * 2, true); str(8, 'WAVE'); str(12, 'fmt ');
  v.setUint32(16, 16, true); v.setUint16(20, 1, true); v.setUint16(22, 1, true); v.setUint32(24, RATE, true);
  v.setUint32(28, RATE * 2, true); v.setUint16(32, 2, true); v.setUint16(34, 16, true); str(36, 'data'); v.setUint32(40, len * 2, true);
  for (let i = 0; i < len; i++) {
    let x = d[i] * g;
    if (i >= len - fade) x *= (len - i) / fade;
    v.setInt16(44 + i * 2, Math.max(-32768, Math.min(32767, Math.round(x * 32767))), true);
  }
  return { blob: new Blob([ab], { type: 'audio/wav' }), secs, cut: audio.duration > maxSec + 0.05 };
}

/*  Заставка й звуки подій — у розділі «Розробник» (так попросив власник).
    Повертає картки й функцію оновлення з S.  */
function sfxSection() {
  let list = null, user = -1, mask = -1;
  const rowsSplash = h('div'), rowsEv = h('div'), space = h('p', { class: 'note' }, '');
  const noFs = h('p', { class: 'note bad', hidden: true },
    'На радіо ще немає розділу ресурсів (заставка, звуки). Його додає прошивка через USB (flash.sh --app-only --assets); станції, мережі й налаштування при цьому лишаються.');
  const volBox = (onSet) => {
    const val = h('span', { class: 'note', style: { margin: 0, minWidth: '44px', textAlign: 'right' } }, '');
    let tm = 0;
    const r = range(0, 100, 60, v => { val.textContent = v ? v + '%' : 'вимк.'; clearTimeout(tm); tm = setTimeout(() => onSet(v), 250); });
    const el = h('div', { class: 'bar', style: { minWidth: '220px', gap: '10px', flex: '1', maxWidth: '360px' } }, r, val);
    el.set = v => { if (!held(r)) { r.value = v; fillRange(r); val.textContent = v ? v + '%' : 'вимк.'; } };
    return el;
  };
  const splashSw = sw(false, v => setx({ splashOff: v ? 0 : 1 }));
  const splashVol = volBox(v => setx({ splashVol: v }));
  const sfxSw = sw(false, v => setx({ sfxOn: v ? 1 : 0 }));
  const sfxVol = volBox(v => setx({ sfxVol: v }));

  function uploadBtn(e) {
    const pick = h('input', { type: 'file', accept: 'audio/*,.wav,.mp3,.ogg,.m4a,.aac,.flac', hidden: true });
    pick.addEventListener('change', async () => {
      const f = pick.files[0]; if (!f) return;
      try {
        toast('Готую звук…');
        if (f.size > SFX_MAX_FILE) throw new Error(`файл ${size(f.size)} — завеликий (до ${size(SFX_MAX_FILE)})`);
        const w = await sfxToWav(f, SFX_MAX_SEC);
        const fd = new FormData(); fd.append('file', w.blob, e.id + '.wav');
        await api('/api/sfx?e=' + e.id, { method: 'POST', body: fd });
        toast(`«${e.t}»: свій звук, ${w.secs.toFixed(1)} с${w.cut ? ` (обрізано до ${SFX_MAX_SEC} с)` : ''}`);
        setTimeout(() => { setx({ sfxPlay: e.id }); reload(); }, 400);
      } catch (err) { toast('Не вдалося: ' + err.message, true); }
      pick.value = '';
    });
    const b = btn('Свій', 'upload', () => pick.click(), 'sm');
    b.title = `MP3 або WAV (також OGG, M4A, FLAC), до ${size(SFX_MAX_FILE)}; звучить перші ${SFX_MAX_SEC} с`;
    return [b, pick];
  }
  const durTxt = e => e.ms ? (e.ms < 1000 ? e.ms + ' мс' : (e.ms / 1000).toFixed(1) + ' с') : '';
  function draw() {
    if (!list) return;
    rowsSplash.textContent = ''; rowsEv.textContent = '';
    list.ev.forEach((e, i) => {
      const bit = 1 << i, mine = !!(user & bit);
      const tail = h('div', { class: 'bar', style: { gap: '6px', flexWrap: 'wrap', justifyContent: 'flex-end' } },
        btn('Прослухати', 'play', () => { setx({ sfxPlay: e.id }); toast(`«${e.t}» звучить на радіо`); }, 'sm'), ...uploadBtn(e),
        mine ? btn('Стандартний', 'refresh', () => { setx({ sfxReset: e.id }); setTimeout(reload, 600); }, 'sm ghost') : null);
      tail.firstChild.title = 'Програти цей звук на радіо з поточною гучністю';
      const sub = `${mine ? 'свій звук' : 'стандартний'}${durTxt(e) ? ', ' + durTxt(e) : ''}`;
      /*  гучність саме цього звуку (привітання — своя; решта — від загальної)  */
      const vb = volBox(v => { setx({ sfxEvVol: `${e.id}:${v}` }); setTimeout(() => setx({ sfxPlay: e.id }), 300); });
      vb.set(e.vol == null ? 100 : e.vol);
      if (e.id === 'start') {
        rowsSplash.append(row('Привітання (звук заставки)', 'грає разом з анімацією · ' + sub, tail));   /* гучність — повзунок splashVol вище */
      } else {
        if (e.id !== 'battery') tail.append(sw(!!(mask & bit), v => setx({ sfxMask: v ? (mask | bit) : (mask & ~bit) })));
        rowsEv.append(row(e.t, `${SFX_SUB[e.id] || ''} · ${sub}`, tail));
        rowsEv.append(row('', `гучність «${e.t.toLowerCase()}» — від загальної`, vb));
      }
    });
    space.textContent = list.fs ? `Розділ ресурсів: зайнято ${size(list.used)} з ${size(list.total)}.` : '';
  }
  async function reload() { try { list = await api('/api/sfx'); draw(); } catch (e) {} }
  reload();
  const els = [
    card('Заставка', row('Анімована заставка', 'при увімкненні, поки радіо шукає мережу', splashSw),
      row('Гучність привітання', 'звук заставки при увімкненні; 0 — без звуку', splashVol), rowsSplash,
      h('div', { class: 'bar', style: { marginTop: '10px' } }, btn('Показати на радіо', 'play', () => { setx({ splashDemo: 7000 }); toast('Дивіться на екран радіо'); }, 'sm'))),
    card('Звуки подій', row('Звуки подій', 'короткі сигнали: жест прийнято, мережа, таймер сну, будильник', sfxSw),
      row('Загальна гучність', 'своя — не залежить від гучності станції; під час звуку станція ненадовго стишується', sfxVol), rowsEv,
      h('div', { class: 'note', style: { marginTop: '12px' } },
        h('b', null, 'Свій звук — кнопка «Свій». '),
        `Підходять MP3 і WAV (також OGG, M4A, FLAC — усе, що відкриває браузер), файл до ${size(SFX_MAX_FILE)}. `,
        `Сторінка сама переводить звук у формат радіо: WAV, моно, 22 050 Гц, 16 біт, до ${SFX_MAX_SEC} с (≈ 440 КБ), `,
        'вирівнює гучність (пік −3 дБ) і м\'яко гасить кінець. Довший звук обрізається. Якість — як у телефонного дзвінка, ',
        'але повнішого: для сигналів і коротких фраз досить; музика довша за 10 с не підходить.'),
      space, noFs),
  ];
  const upd = () => {
    if (!S || !S.sfx) return;
    const x = S.sfx;
    splashSw.firstChild.checked = !x.splashOff; splashVol.set(x.splashVol);
    sfxSw.firstChild.checked = !!x.on; sfxVol.set(x.vol);
    noFs.hidden = !!x.fs;
    if (x.mask !== mask || x.user !== user) { const again = user !== -1 && x.user !== user; mask = x.mask; user = x.user; again ? reload() : draw(); }
  };
  return { els, upd };
}

/* ---------------------------------------------------------------- голосові команди: інструкція
   Що казати — з того самого розбору, що виконує команди (voiceParse), тож
   список і поведінка не розійдуться. Фразу можна перевірити й без мікрофона.  */
const VOICE_HELP = [
  ['Відтворення', [['пауза', 'стоп', 'вимкни'], 'зупинити'], [['грай', 'увімкни', 'продовжуй'], 'грати далі']],
  ['Станції', [['наступна', 'далі'], 'наступна станція'], [['попередня', 'назад'], 'попередня станція'],
    [['грай Хіт FM', 'увімкни Relax'], 'станція за назвою — як у списку, можна неточно'], [['станція 5', 'номер 12'], 'станція за номером у списку']],
  ['Обране', [['обране 2', 'кнопка 3'], 'одна з шести кнопок обраного']],
  ['Гучність', [['гучність 30'], 'гучність у відсотках, від 0 до 100'], [['голосніше', 'тихіше'], 'на крок (близько 10 %)'], [['без звуку', 'тиша'], 'вимкнути звук']],
  ['Звук', [['еквалайзер голос', 'режим звуку бас'], 'готові налаштування: рівно, голос, музика, бас, ніч, яскраво, тепло']],
  ['Таймер сну', [['таймер сну 30 хвилин', 'засни через годину', 'вимкни через півтори години'], 'радіо затихне через цей час'], [['скасуй таймер'], 'прибрати таймер']],
  ['Будильник', [['будильник на 7', 'будильник на 6:30', 'будильник на пів на восьму', 'будильник на 9 вечора'], 'поставити й увімкнути будильник'], [['вимкни будильник'], 'вимкнути']],
  ['Проповіді', [['остання проповідь'], 'найновіша з архіву'], [['проповідь про надію', 'проповідь пастора Петра'], 'за назвою чи проповідником'],
    [['наступна проповідь', 'попередня проповідь'], 'сусідня в архіві; поки грає проповідь, досить «наступна» чи «назад»']],
  ['Що грає', [['що грає', 'що це'], 'назва станції й пісні чи проповіді — з\'явиться внизу сторінки']],
];

VIEWS.voice = page => {
  const br = voiceBridge();
  const how = card('Як користуватись',
    h('p', { class: 'vh-lead' }, br
      ? h('span', null, 'Натисніть кнопку ', h('span', { class: 'vh-mic' }, ic('mic')), ' угорі сторінки й скажіть команду — коротко, як радіо: «наступна», «гучність 30».')
      : 'Голосові команди працюють у програмах ПОТУЖНЕ РАДІО для Android і Mac: там угорі сторінки є кнопка з мікрофоном. У браузері її немає — сторінка радіо не має доступу до мікрофона. Фразу можна перевірити й тут, унизу сторінки.'),
    h('ul', { class: 'vh-list' },
      h('li', null, 'Мову розпізнає сам телефон (Google) чи Mac (Apple), тому потрібен інтернет. Розуміє українську; російські фрази теж спрацюють.'),
      h('li', null, 'Поки слухає, кнопка світиться, а внизу видно почуте. Скажіть і замовкніть — за секунду тиші радіо виконає команду й напише, що зробило.'),
      h('li', null, 'Не зрозуміло — радіо так і скаже й підкаже приклади. Назву станції кажіть так, як вона записана в списку; точність не обов\'язкова.'),
      h('li', null, 'Першого разу програма попросить дозвіл на мікрофон, а на Mac — ще й на розпізнавання мовлення. Якщо відмовили: Android — Налаштування → Програми → ПОТУЖНЕ РАДІО → Дозволи → Мікрофон; Mac — Системні параметри → Приватність і безпека → Мікрофон та Розпізнавання мовлення.'),
      h('li', null, 'У програмі для Windows голосових команд поки немає.')));
  const groups = card('Що можна сказати');
  for (const [title, ...items] of VOICE_HELP) {
    const g = h('div', { class: 'vh-grp' }, h('h3', null, title));
    for (const [phrases, what] of items) {
      g.append(h('div', { class: 'vh-item' },
        h('div', { class: 'vh-say' }, ...phrases.map(ph => h('button', { type: 'button', class: 'vh-chip', title: 'перевірити цю фразу', onclick: () => { inp.value = ph; check(); inp.focus(); } }, '«' + ph + '»'))),
        h('div', { class: 'vh-what' }, what)));
    }
    groups.append(g);
  }
  const inp = h('input', { type: 'text', placeholder: 'наприклад: грай Хіт FM', 'aria-label': 'Фраза', enterkeyhint: 'go' });
  const out = h('p', { class: 'note vh-out' }, 'Напишіть фразу чи торкніться прикладу вище — радіо покаже, що зробило б, нічого не міняючи.');
  let last = '';
  async function check() {
    const t = inp.value.trim();
    if (!t) return;
    last = t;
    const r = await window.potuzhneVoiceTest(t, true);
    if (inp.value.trim() !== t) return;
    /*  Не кожна відповідь — дія: «архів ще вантажиться», «у кнопці нічого немає».  */
    const info = r.done && /^(архів проповідей|у кнопці обраного|проповіді «)/.test(r.done);
    if (r.done && r.done.startsWith('архів проповідей')) vsermons();      /* справді попросити радіо завантажити архів */
    out.textContent = !r.done ? 'Цю фразу радіо не зрозуміє. Спробуйте простіше — як у прикладах вище.'
      : info ? `Радіо відповість: ${r.done}.` : `Радіо зробить: ${r.done}.`;
    out.classList.toggle('bad', !r.done);
    run.disabled = !r.done || !!info;
  }
  const run = btn('Виконати', 'play', async () => {
    const t = inp.value.trim();
    if (!t) return;
    const done = await voiceParse(t);
    if (done) { setTimeout(pollState, 700); toast(`«${t}» → ${done}`); } else toast('Цю фразу радіо не зрозуміє', true);
  });
  run.disabled = true;
  inp.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); check(); } });
  inp.addEventListener('input', () => { if (inp.value.trim() !== last) { run.disabled = true; } });
  const tryit = card('Перевірити фразу',
    h('div', { class: 'vh-try' }, inp, btn('Перевірити', 'check', check, 'acc'), run), out);
  page.append(...(br === 'mac' ? [voicePermCard()] : []), how, tryit, groups);
};

/* ---------------------------------------------------------------- голос
   Мову розпізнає застосунок (Android — Google, Mac — Apple): сторінка по
   http до мікрофона доступу не має. Застосунок віддає сюди варіанти
   фрази, а тут вони стають командами радіо — однаково для всіх програм.  */
const VOICE = { busy: false, ov: null, sermons: null };

function voiceBridge() {
  if (window.PotuzhneApp && typeof window.PotuzhneApp.listen === 'function') return 'android';
  if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.voice) return 'mac';
  return '';
}

/*  Дозволи на Mac: стан питаємо в програми ('perm'), вона відповідає
    potuzhneVoicePerm({mic, speech, device, language, available, ready}) — і сама
    надсилає знову, коли людина повертається з «Системних параметрів».
    Стара програма не відповідає — тоді підказка оновити її.  */
const voicePost = m => { try { window.webkit.messageHandlers.voice.postMessage(m); } catch (e) {} };
function voicePermCard() {
  const box = h('div', null, h('p', { class: 'note', style: { margin: 0 } }, 'Питаю програму…'));
  VOICE.permBox = box; VOICE.permGot = false;
  voicePost('perm');
  setTimeout(() => {
    if (VOICE.permGot || !box.isConnected) return;
    box.textContent = '';
    box.append(h('p', { class: 'note', style: { margin: 0 } }, 'Ця версія програми для Mac не вміє перевіряти дозволи. Завантажте нову з GitHub (файл PotuzhneRadio-Mac.zip у розділі «Оновлення» → «З GitHub»).'));
  }, 2500);
  return card('Дозволи на цьому Mac', box, h('div', { class: 'bar', style: { marginTop: '12px' } },
    btn('Перевірити знову', 'refresh', () => voicePost('perm'), 'sm'),
    btn('Спитати дозвіл знову', 'mic', () => voicePost('askAgain'), 'sm ghost'),
    btn('Вікно перевірки', 'info', () => voicePost('window'), 'sm ghost')),
    h('p', { class: 'note' }, 'Дозволи macOS питає лише раз. Якщо колись натиснули «Не дозволяти» або програми немає в списку налаштувань — «Спитати дозвіл знову»: macOS забуде старе рішення й покаже запит.'));
}
window.potuzhneVoicePerm = p => {
  VOICE.permGot = true;
  const box = VOICE.permBox;
  if (!box || !box.isConnected || !p) return;
  const T = { granted: 'дозволено', denied: 'заборонено', restricted: 'заборонено політикою Mac', ask: 'ще не питали' };
  const pill = (ok, text) => h('span', { class: 'pill ' + (ok ? 'ok' : 'bad') }, text);
  const access = (a, pane) => a === 'granted' ? null : a === 'ask'
    ? btn('Дозволити', 'check', () => voicePost('ask'), 'sm acc')
    : btn('Відкрити налаштування', 'gear', () => voicePost('open:' + pane), 'sm acc');
  const langOk = p.language && p.available;
  box.textContent = '';
  box.append(
    h('p', { class: 'note', style: { marginTop: 0, color: p.ready ? 'var(--ok)' : 'var(--bad)' } },
      p.ready ? 'Усе готово: натисніть кнопку з мікрофоном угорі сторінки й скажіть команду.' : 'Програмі бракує дозволу чи умови — кнопка в рядку веде просто в потрібні налаштування.'),
    row('Мікрофон', p.mic === 'granted' ? null : 'Системні параметри → Приватність і безпека → Мікрофон → «ПОТУЖНЕ РАДІО»',
      h('div', { class: 'bar' }, pill(p.mic === 'granted', T[p.mic] || p.mic), access(p.mic, 'mic'))),
    row('Розпізнавання мовлення', p.speech === 'granted' ? null : 'Системні параметри → Приватність і безпека → Розпізнавання мовлення → «ПОТУЖНЕ РАДІО»',
      h('div', { class: 'bar' }, pill(p.speech === 'granted', T[p.speech] || p.speech), access(p.speech, 'speech'))),
    row('Пристрій запису', p.device || 'мікрофона не знайдено — під\'єднайте його чи виберіть у налаштуваннях звуку',
      h('div', { class: 'bar' }, pill(!!p.device, p.device ? 'є' : 'немає'), btn('Налаштування звуку', null, () => voicePost('open:sound'), 'sm ' + (p.device ? 'ghost' : 'acc')))),
    row('Українська мова', !p.language ? 'цей Mac її не розпізнає' : p.available ? 'розпізнає Apple через інтернет' : 'зараз недоступна: потрібен інтернет; якщо він є — увімкніть диктування',
      h('div', { class: 'bar' }, pill(langOk, langOk ? 'доступна' : 'недоступна'), p.language && !p.available ? btn('Диктування', null, () => voicePost('open:dictation'), 'sm acc') : null)));
};

function voiceSetup() {
  const b = $('#voicebtn');
  if (b) b.hidden = !voiceBridge();
}

const VOICE_HINT = 'наприклад: «наступна», «пауза», «гучність 30», «грай Хіт FM», «таймер сну 30 хвилин», «будильник на 7», «остання проповідь»';

function voiceOverlay(text, sub) {
  if (!VOICE.ov) {
    VOICE.ov = h('div', { class: 'voice-ov', role: 'status' }, h('i', { class: 'voice-dot' }), h('div', null, h('b'), h('small'),
      h('a', { href: '#/voice', class: 'voice-all' }, 'усі команди')));
    VOICE.ov.addEventListener('click', () => { VOICE.ov.hidden = true; });
    document.body.append(VOICE.ov);
  }
  VOICE.ov.querySelector('b').textContent = text;
  VOICE.ov.querySelector('small').textContent = sub || '';
  VOICE.ov.hidden = false;
}

function voiceStart() {
  const br = voiceBridge();
  if (!br || VOICE.busy) return;
  VOICE.busy = true;
  $('#voicebtn') && $('#voicebtn').classList.add('on');
  voiceOverlay('Слухаю…', VOICE_HINT);
  try {
    if (br === 'android') window.PotuzhneApp.listen();
    else window.webkit.messageHandlers.voice.postMessage('listen');
  } catch (e) { window.potuzhneVoiceError('Застосунок не зміг почати слухати'); }
}

function voiceDone() {
  VOICE.busy = false;
  $('#voicebtn') && $('#voicebtn').classList.remove('on');
}

window.potuzhneVoicePartial = t => { if (VOICE.busy) voiceOverlay('«' + t + '»', 'слухаю…'); };
window.potuzhneVoiceError = msg => { voiceDone(); if (VOICE.ov) VOICE.ov.hidden = true; toast(msg, true); };
window.potuzhneVoiceResult = list => {
  voiceDone();
  const alts = (Array.isArray(list) ? list : [list]).map(x => String(x || '').trim()).filter(Boolean);
  if (!alts.length) { if (VOICE.ov) VOICE.ov.hidden = true; toast('Нічого не почуто', true); return; }
  voiceRun(alts).then(r => {
    if (VOICE.ov) VOICE.ov.hidden = true;
    if (r) toast(`«${r.said}» → ${r.done}`);
    else toast(`Не зрозумів «${alts[0]}». Скажіть, ${VOICE_HINT}. Усі команди — у розділі «Голосові команди».`, true);
  });
};

/* ---------- розбір ---------- */

const vnorm = s => String(s).toLowerCase().replace(/ё/g, 'е').replace(/[’ʼ`´]/g, "'").replace(/[^\p{L}\p{N}' :]+/gu, ' ').replace(/\s+/g, ' ').trim();

const VNUM = {
  'нуль': 0, 'один': 1, 'одна': 1, 'одну': 1, 'одне': 1, 'перша': 1, 'перше': 1, 'першу': 1, 'первая': 1, 'первую': 1,
  'два': 2, 'дві': 2, 'две': 2, 'друга': 2, 'друге': 2, 'другу': 2, 'вторая': 2, 'вторую': 2,
  'три': 3, 'третя': 3, 'третє': 3, 'третю': 3, 'третья': 3, 'третью': 3,
  'чотири': 4, 'четыре': 4, 'четверта': 4, 'четверте': 4, 'четвертая': 4,
  "п'ять": 5, 'пять': 5, "п'ята": 5, 'пятая': 5, 'шість': 6, 'шесть': 6, 'шоста': 6, 'шестая': 6,
  'сім': 7, 'семь': 7, 'сьома': 7, 'сьому': 7, 'седьмая': 7, 'вісім': 8, 'восемь': 8, 'восьма': 8, 'восьму': 8,
  "дев'ять": 9, 'девять': 9, "дев'яту": 9, "дев'ята": 9, 'десять': 10, 'десяту': 10, 'десята': 10, "одинадцять": 11, 'одиннадцать': 11,
  'шосту': 6, "п'яту": 5, 'четверту': 4, 'четвертую': 4, 'сьомої': 7, 'одинадцяту': 11, 'дванадцяту': 12, 'шестую': 6, 'пятую': 5, 'седьмую': 7, 'восьмую': 8,
  'дванадцять': 12, 'двенадцать': 12, 'тринадцять': 13, 'тринадцать': 13, 'чотирнадцять': 14, 'четырнадцать': 14,
  "п'ятнадцять": 15, 'пятнадцать': 15, 'шістнадцять': 16, 'шестнадцать': 16, 'сімнадцять': 17, 'семнадцать': 17,
  'вісімнадцять': 18, 'восемнадцать': 18, "дев'ятнадцять": 19, 'девятнадцать': 19,
  'двадцять': 20, 'двадцать': 20, 'тридцять': 30, 'тридцать': 30, 'сорок': 40, "п'ятдесят": 50, 'пятьдесят': 50,
  'шістдесят': 60, 'шестьдесят': 60, 'сімдесят': 70, 'семьдесят': 70, 'вісімдесят': 80, 'восемьдесят': 80,
  "дев'яносто": 90, 'девяносто': 90, 'сто': 100,
};

/*  перше число у фразі: цифрами або словами («двадцять п'ять»)  */
function vnum(s) {
  const m = s.match(/\d+/);
  if (m) return +m[0];
  let total = null;
  for (const w of s.split(' ')) {
    if (w in VNUM) { total = (total || 0) + VNUM[w]; }
    else if (total !== null) break;
  }
  return total;
}

/*  латиниця для порівняння назв: «хіт фм» і «Hit FM» мають зустрітись  */
const VTR = { 'а': 'a', 'б': 'b', 'в': 'v', 'г': 'h', 'ґ': 'g', 'д': 'd', 'е': 'e', 'є': 'ie', 'ж': 'zh', 'з': 'z', 'и': 'y', 'і': 'i', 'ї': 'i', 'й': 'i',
  'к': 'k', 'л': 'l', 'м': 'm', 'н': 'n', 'о': 'o', 'п': 'p', 'р': 'r', 'с': 's', 'т': 't', 'у': 'u', 'ф': 'f', 'х': 'h', 'ц': 'ts', 'ч': 'ch',
  'ш': 'sh', 'щ': 'sch', 'ь': '', 'ю': 'iu', 'я': 'ia', 'ы': 'y', 'э': 'e', 'ъ': '', "'": '' };
const vlat = s => vnorm(s).split('').map(c => VTR[c] !== undefined ? VTR[c] : c).join('')
  .replace(/ph/g, 'f').replace(/w/g, 'v').replace(/c(?=[eiy])/g, 's').replace(/c/g, 'k').replace(/q/g, 'k').replace(/x/g, 'ks').replace(/g/g, 'h')
  .replace(/dzh|dj/g, 'j').replace(/([a-z])\1+/g, '$1');

function vlev(a, b) {
  if (a === b) return 0;
  const m = a.length, n = b.length;
  if (!m) return n; if (!n) return m;
  let prev = Array.from({ length: n + 1 }, (_, j) => j);
  for (let i = 1; i <= m; i++) {
    const cur = [i];
    for (let j = 1; j <= n; j++) cur[j] = Math.min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
    prev = cur;
  }
  return prev[n];
}
const vsim = (a, b) => 1 - vlev(a, b) / Math.max(a.length, b.length, 1);

/*  наскільки фраза схожа на назву: кожне слово фрази шукаємо серед слів назви  */
/*  слова, що є в половині назв, — не ознака саме цієї станції  */
const VGENERIC = new Set(['radio', 'radiio', 'radyo', 'fm', 'am', 'hd', 'stantsiia', 'stantsia', 'stantsyia', 'kanal', 'online', 'the']);
function vscore(query, name) {
  const cut = w => w.filter(x => !VGENERIC.has(x));
  let q = vlat(query).split(' ').filter(w => w.length > 1), nm = vlat(name).split(' ').filter(Boolean);
  if (cut(q).length) q = cut(q);
  if (cut(nm).length) nm = cut(nm);
  if (!q.length || !nm.length) return 0;
  const joined = nm.join(''), qj = q.join('');
  if (joined.includes(qj) && qj.length >= 3) return 0.95;
  let sum = 0;
  for (const w of q) sum += Math.max(...nm.map(x => vsim(w, x)), vsim(w, joined));
  return Math.max(sum / q.length, vsim(qj, joined));
}

function vbest(query, items, key) {
  let best = null, bs = 0;
  items.forEach((it, i) => {
    const s = vscore(query, key(it));
    if (s > bs + 0.001 || (Math.abs(s - bs) <= 0.001 && best !== null && key(it).length < key(items[best]).length)) { bs = s; best = i; }
  });
  return bs >= 0.62 ? { i: best, score: bs } : null;
}

async function vsermons() {
  if (VOICE.sermons && VOICE.sermons.length) return VOICE.sermons;
  try {
    const d = await api('/api/sermons');
    if (d.items && d.items.length) { VOICE.sermons = d.items; return d.items; }
    if (!d.load) setx({ sermLoad: 1 });
  } catch (e) {}
  return null;
}

const V_PRESETS = [['рівно', 1], ['ровно', 1], ['голос', 2], ['музика', 3], ['музыка', 3], ['бас', 4], ['ніч', 5], ['ночь', 5], ['нічний', 5], ['яскраво', 6], ['ярко', 6], ['тепло', 7]];

/*  Одна фраза → дія. null — не зрозуміло.  */
async function voiceParse(raw) {
  const s = vnorm(raw);
  if (!s) return null;
  const has = re => re.test(s);
  const playing = !!W.playing;
  const serm = isSerm();

  /*  таймер сну  */
  if (has(/таймер|засн|сон через|сна через|вимкн\S* через|выключ\S* через/)) {
    if (has(/скасу|вимкни таймер|прибери|отмен|выключи таймер|без таймера/)) { setx({ sleep: 0 }); return 'таймер сну вимкнено'; }
    let min = vnum(s);
    if (has(/півтор|полтор/)) min = 90;
    else if (has(/пів ?годин|полчаса/)) min = 30;
    else if (has(/годин|час/) && min !== null) min *= 60;
    else if (has(/годин|час/)) min = 60;
    if (!min) return null;
    min = Math.max(1, Math.min(600, min));
    setx({ sleep: min });
    return `таймер сну на ${min} хв`;
  }
  /*  будильник  */
  if (has(/будильник|разбуди|розбуди/)) {
    if (has(/вимкн|скасу|выключ|отмен|не треба/)) { setx({ alarmOn: 0 }); return 'будильник вимкнено'; }
    const t = s.match(/(\d{1,2})(?:[: ](\d{2}))?/);
    let hh = t ? +t[1] : vnum(s), mm = t && t[2] ? +t[2] : 0;
    if (hh === null || hh > 23) return null;
    if (has(/пів ?на|половин/) && !t) { hh = Math.max(0, hh - 1); mm = 30; }
    if (has(/вечора|вечера/) && hh < 12) hh += 12;
    setx({ alarmH: hh, alarmM: mm, alarmOn: 1 });
    return `будильник на ${String(hh).padStart(2, '0')}:${String(mm).padStart(2, '0')}`;
  }
  /*  гучність  */
  if (has(/гучніш|голосніш|громче|погромче|сильніше/)) { const v = Math.min(254, (W.vol | 0) + 25); send('volume=' + v); return `гучність ${Math.round(v / 2.54)}%`; }
  if (has(/тихіш|тише|потише|слабше/)) { const v = Math.max(0, (W.vol | 0) - 25); send('volume=' + v); return `гучність ${Math.round(v / 2.54)}%`; }
  if (has(/гучність|громкость|звук на/)) {
    const n = vnum(s);
    if (n === null) return null;
    const v = Math.round(Math.max(0, Math.min(100, n)) * 2.54);
    send('volume=' + v);
    return `гучність ${Math.min(100, n)}%`;
  }
  if (has(/^(без звуку|вимкни звук|выключи звук|тиша)$/)) { send('volume=0'); return 'без звуку'; }
  /*  еквалайзер  */
  if (has(/пресет|еквалайзер|эквалайзер|режим звуку|звук /)) {
    const p = V_PRESETS.find(([w]) => s.includes(w));
    if (p) { setx({ eqPreset: p[1] }); return `звук: ${p[0]}`; }
  }
  /*  що грає  */
  if (has(/що (зараз )?грає|що це|что (сейчас )?играет|что это/)) {
    return serm && S.serm.t ? `проповідь «${S.serm.t}»` : `${W.name || 'нічого не грає'}${W.title ? ' — ' + W.title : ''}`;
  }
  /*  наступна / попередня проповідь — навіть якщо зараз грає радіо  */
  if (has(/проповід|проповед/) && has(/наступн|следующ|далі|дальше/)) { setx({ sermRel: 1 }); return 'наступна проповідь'; }
  if (has(/проповід|проповед/) && has(/попередн|предыдущ|назад/)) { setx({ sermRel: -1 }); return 'попередня проповідь'; }
  /*  наступна / попередня  */
  if (has(/^(наступн|далі|дальше|следующ|вперед|next)/) || has(/^(перемкни|переключи)/)) {
    serm ? setx({ sermRel: 1 }) : send('next=1');
    return serm ? 'наступна проповідь' : 'наступна станція';
  }
  if (has(/^(попередн|назад|предыдущ|previous)/)) {
    serm ? setx({ sermRel: -1 }) : send('prev=1');
    return serm ? 'попередня проповідь' : 'попередня станція';
  }
  /*  пауза / грати  */
  if (has(/^(пауза|стоп|зупини\S*|вимкни|вимкни радіо|останови\S*|стой|выключи|выключи радио|тихо)$/)) {
    if (playing) send('toggle=1');
    return 'пауза';
  }
  if (has(/^(грай|грати|увімкни|увімкни радіо|включи|включи радио|продовж\S*|продолж\S*|играй|пуск|старт|play)$/)) {
    if (!playing) send('toggle=1');
    return 'грає';
  }
  /*  обране  */
  if (has(/обран|избран|кнопк/)) {
    const n = vnum(s);
    if (n && n >= 1 && n <= 6) {
      if (S && S.fav && S.fav[n - 1] && S.fav[n - 1].n) { setx({ favPlay: n - 1 }); return `обране ${n}: ${S.fav[n - 1].n}`; }
      return `у кнопці обраного ${n} нічого немає`;
    }
  }
  /*  проповідь  */
  if (has(/проповід|проповед/)) {
    const items = await vsermons();
    if (!items) return 'архів проповідей ще вантажиться — скажіть за хвилину';
    const rest = s.replace(/.*?(проповід\S*|проповед\S*)\s*/, '').replace(/^(про|на тему|тему|від|пастора|брата)\s+/, '').trim();
    if (!rest || has(/остан|свіж|нов|последн/)) { setx({ serm: 0 }); return `проповідь «${items[0].t}»`; }
    const b = vbest(rest, items, it => `${it.t} ${it.p || ''}`);
    if (!b) return `проповіді «${rest}» не знайшов`;
    setx({ serm: b.i });
    return `проповідь «${items[b.i].t}»`;
  }
  /*  станція за номером або назвою  */
  const m = s.match(/^(?:грай|грати|увімкни|включи|постав\S*|станці\S*|радіо|радио|играй|хочу|давай)\s+(.+)$/);
  const q = m ? m[1] : s;
  const num = q.match(/^(?:номер\s+)?(\d+)$/) || (m && /^номер/.test(q) && vnum(q) ? [0, vnum(q)] : null);
  if (num && PL.length && +num[1] >= 1 && +num[1] <= PL.length) { send('play=' + num[1]); return `станція ${num[1]}: ${PL[num[1] - 1].name}`; }
  if (PL.length) {
    const b = vbest(q, PL, x => x.name);
    if (b && (m || b.score >= 0.8)) { send('play=' + (b.i + 1)); return `грає «${PL[b.i].name}»`; }
  }
  return null;
}

async function voiceRun(alts) {
  for (const a of alts) {
    const done = await voiceParse(a);
    if (done) { setTimeout(pollState, 700); return { said: a, done }; }
  }
  return null;
}
/*  перевірка з консолі: dry — лише показати, що зробилося б, нічого не надсилаючи  */
window.potuzhneVoiceTest = async (text, dry = true) => {
  const acts = [], s0 = send, x0 = setx;
  if (dry) { send = c => { acts.push(c); }; setx = o => { acts.push(JSON.stringify(o)); return Promise.resolve(); }; }
  try { return { done: await voiceParse(text), acts }; } finally { if (dry) { send = s0; setx = x0; } }
};

/* ---------------------------------------------------------------- старт */
function start() {
  shell();
  window.addEventListener('hashchange', route);
  route();
  wsConnect(); loadPlayList(); loadLogos();
  pollState();
  voiceSetup();
  setTimeout(voiceSetup, 1500);                 /* міст застосунку може з'явитись трохи пізніше */
  /*  гучність із клавіатури: стрілки вгору/вниз, пробіл — пауза  */
  document.addEventListener('keydown', e => {
    if (e.target.closest('input,select,textarea,.dlg') || e.metaKey || e.ctrlKey || e.altKey) return;
    if (e.key === ' ') { e.preventDefault(); send('toggle=1'); }
  });
}
start();
