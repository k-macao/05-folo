/* 005 · SIGNAL.LOG —— 240x320 逻辑像素预览。
 *
 * 这里的排版常量、换行规则、时间校验和详情页结构，与固件中的
 * main/ui_crt.c + main/intel_logic.c + main/intel_feed.c 保持一致：
 *   - 半角 6 px / 全角 12 px，每行 38 格（列表）、36 格（详情）
 *   - 时间不可信（MISSING）、无法解析、比 generated_at 晚 10 分钟以上的条目一律丢弃
 *   - 列表 6 行 × 46 px，详情 17 行 × 15 px，顶栏 16 px，脚注 304 px
 */
(() => {
  'use strict';

  const W = 240;
  const H = 320;
  const STATUS_H = 16;
  const ROW_H = 46;
  const VISIBLE_ROWS = 6;
  const LIST_TOP = 18;
  const DETAIL_TOP = 33;
  const DETAIL_ROWS = 17;
  const FOOTER_RULE = 302;
  const FOOTER_Y = 304;
  const LIST_CELLS = 38;
  const DETAIL_CELLS = 36;
  const MAX_ITEMS = 12;

  const C = {
    bg: '#000000',
    fg: '#3bff7c',
    dim: '#1e9e52',
    bar: '#00ff66',
    ink: '#001400',
    amber: '#ffc24a',
    red: '#ff5a4a',
    cyan: '#58d8ff',
    white: '#e8ffe8',
  };

  const GRADE_COLOR = { EXACT: C.fg, DATE: C.amber, DERIVED: '#ff9a3c' };

  const screenEl = document.getElementById('screen');
  const stageEl = document.getElementById('screenStage');
  const factsEl = document.getElementById('facts');

  const state = {
    view: 'boot',
    feed: null,
    selected: 0,
    detailIndex: 0,
    detailFirst: 0,
    busy: false,
    stale: false,
    err: '等待首次同步',
    dropped: 0,
    syncAt: 0,
    offline: false,
    empty: false,
    bytes: 0,
    battery: 87,
  };

  // --------------------------------------------------------------- utilities
  const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  const NORMALISE = {
    '\u00a0': ' ',
    '\u2014': '-', '\u2015': '-', '\u2500': '-',
    '\u2018': "'", '\u2019': "'", '\u2032': "'",
    '\u201c': '"', '\u201d': '"',
    '\u00b0': 'o',
    '\u2713': 'v', '\u2714': 'v',
    '\u2715': 'x', '\u2717': 'x', '\u2718': 'x',
  };

  function normalize(input) {
    if (!input) return '';
    let out = '';
    let pendingSpace = false;
    for (const ch of String(input)) {
      const code = ch.codePointAt(0);
      if (code < 0x20) continue;
      if (code >= 0x370 && code <= 0x3ff) { out += '?'; continue; }
      const mapped = NORMALISE[ch] || ch;
      if (mapped === ' ' || ch === '\t' || ch === '\n' || ch === '\r' || ch === '\u3000') {
        if (out.length) pendingSpace = true;
        continue;
      }
      if (pendingSpace) { out += ' '; pendingSpace = false; }
      out += mapped;
    }
    return out;
  }

  const isWide = (cp) =>
    cp >= 0x80; // 与固件 intel_cp_cells 一致：非 ASCII 一律按全角 12 px 计

  function cells(str) {
    let n = 0;
    for (const ch of str) n += isWide(ch.codePointAt(0)) ? 2 : 1;
    return n;
  }

  function wrap(text, maxCells, maxLines) {
    const src = text || '';
    if (!src) return [];
    const lines = [];
    let cur = '';
    let used = 0;
    let truncated = false;

    for (const ch of src) {
      const w = isWide(ch.codePointAt(0)) ? 2 : 1;
      if (used + w > maxCells && cur.length) {
        if (lines.length + 1 >= maxLines) { truncated = true; break; }
        lines.push(cur);
        cur = '';
        used = 0;
        if (ch === ' ') continue;
      }
      cur += ch;
      used += w;
    }
    if (cur) lines.push(cur);

    if (truncated && lines.length) {
      let last = lines[lines.length - 1];
      let n = cells(last);
      while (last.length && n + 2 > maxCells) {
        const arr = [...last];
        arr.pop();
        last = arr.join('');
        n = cells(last);
      }
      lines[lines.length - 1] = last + '…';
    }
    return lines;
  }

  function parseIso(value) {
    if (typeof value !== 'string') return 0;
    const m = value.match(/^(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2}):(\d{2})/);
    if (!m) return 0;
    const ms = Date.parse(value);
    return Number.isFinite(ms) ? ms : 0;
  }

  const pad2 = (n) => String(n).padStart(2, '0');

  function hhmm(ms) {
    if (!ms) return '--:--';
    const d = new Date(ms + 8 * 3600 * 1000);
    return pad2(d.getUTCHours()) + ':' + pad2(d.getUTCMinutes());
  }

  function mmdd(ms) {
    const d = new Date(ms + 8 * 3600 * 1000);
    return pad2(d.getUTCMonth() + 1) + '-' + pad2(d.getUTCDate());
  }

  function clockNow() {
    if (!state.feed || !state.feed.generatedEpoch) return 0;
    return state.feed.generatedEpoch + (Date.now() - state.syncAt);
  }

  function ageText(minutes) {
    if (minutes < 0) return '--';
    if (minutes < 1) return '刚刚';
    if (minutes < 60) return `${minutes}分前`;
    if (minutes < 1440) return `${Math.floor(minutes / 60)}时${pad2(minutes % 60)}分`;
    return `${Math.floor(minutes / 1440)}天前`;
  }

  const gradeLabel = (g) => (g === 'EXACT' || g === 'DATE' || g === 'DERIVED' ? g : '--');
  const biasLabel = (b) => ({ bull: '偏多', bear: '偏空', neutral: '中性' }[b] || '--');
  const changeColor = (q) => (!q || !q.hasQuote ? C.dim : q.changePct > 0 ? C.red : q.changePct < 0 ? C.cyan : '#c9c9a8');
  const pctText = (v) => (v >= 0 ? '+' : '') + v.toFixed(2) + '%';

  // ------------------------------------------------------------ feed parsing
  function parseFeed(raw) {
    const items = [];
    let dropped = 0;
    const generatedEpoch = parseIso(raw && raw.generated_at);

    const list = raw && Array.isArray(raw.items) ? raw.items : [];
    for (const it of list) {
      if (items.length >= MAX_ITEMS) { dropped++; continue; }
      const title = normalize(it && it.title);
      if (!title) { dropped++; continue; }

      const epoch = parseIso(it.time);
      if (!epoch) { dropped++; continue; }

      const grade = gradeLabel(String(it.time_grade || '').toUpperCase());
      if (grade === '--') { dropped++; continue; }
      if (generatedEpoch && epoch > generatedEpoch + 10 * 60 * 1000) { dropped++; continue; }

      const tags = (Array.isArray(it.tags) ? it.tags : [])
        .map((t) => normalize(t)).filter(Boolean).slice(0, 3);

      const sec = it.security || {};
      const hasQuote = Number.isFinite(Number(sec.price)) && Number.isFinite(Number(sec.change_pct)) &&
                       sec.price !== null && sec.change_pct !== undefined;

      items.push({
        title,
        oneLine: normalize(it.one_line),
        source: normalize(it.source_name) || '--',
        time: it.time,
        epoch,
        grade,
        tags,
        bias: ['bull', 'bear', 'neutral'].includes(it.bias) ? it.bias : 'none',
        quote: {
          code: normalize(sec.code),
          name: normalize(sec.name),
          hasQuote: Boolean(hasQuote),
          price: hasQuote ? Number(sec.price) : 0,
          changePct: hasQuote ? Number(sec.change_pct) : 0,
        },
        url: typeof it.url === 'string' ? it.url : '',
      });
    }

    if (!items.length) throw new Error(`no usable items (${dropped} dropped)`);

    return {
      source: normalize(raw.source) || '005',
      generatedAt: raw.generated_at || '',
      generatedEpoch,
      windowMinutes: Number(raw.window_minutes) || 180,
      items,
      dropped,
    };
  }

  async function loadFeed() {
    state.busy = true;
    render();
    await sleep(360);
    try {
      if (state.offline) throw new Error('模拟断网');
      if (state.empty) throw new Error('no usable items (0 dropped)');

      const res = await fetch('feed.json?ts=' + Date.now(), { cache: 'no-store' });
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const text = await res.text();
      const feed = parseFeed(JSON.parse(text));

      state.feed = feed;
      state.bytes = new TextEncoder().encode(text).length;
      state.dropped = feed.dropped;
      state.syncAt = Date.now();
      state.stale = false;
      state.selected = clamp(state.selected, 0, feed.items.length - 1);
      if (state.view !== 'detail') state.view = 'list';
    } catch (e) {
      state.stale = true;
      state.err = '拉取失败：' + (e && e.message ? e.message : '未知错误');
      if (!state.feed) state.view = 'error';
    }
    state.busy = false;
    render();
  }

  // ----------------------------------------------------------- detail builder
  function detailLines(item, nowMs) {
    const out = [];
    const push = (text, style) => out.push({ text, style });
    const rule = () => push('-'.repeat(DETAIL_CELLS), 'rule');
    const wrapped = (text, style, maxLines) =>
      wrap(text, DETAIL_CELLS, maxLines).forEach((l) => push(l, style));

    if (item.oneLine) {
      push('AI 一句话', 'head');
      wrapped(item.oneLine, 'accent', 5);
      rule();
    }

    const q = item.quote;
    if (q.hasQuote || q.name || q.code) {
      push(q.code ? `${q.code} ${q.name || '--'}` : (q.name || '--'), 'plain');
      if (q.hasQuote) push(`${q.price.toFixed(2)}  ${pctText(q.changePct)}`, q.changePct >= 0 ? 'bull' : 'bear');
      else push('暂无实时行情', 'dim');
      rule();
    }

    push(`${mmdd(item.epoch)} ${hhmm(item.epoch)} ${item.grade} · ${ageText(Math.max(0, Math.floor((nowMs - item.epoch) / 60000)))}`, 'plain');
    push(`来源 ${item.source}`, 'plain');
    if (item.tags.length) push(`标签 ${item.tags.join(' / ')}`, 'plain');
    push(`倾向 ${biasLabel(item.bias)}`, item.bias === 'none' ? 'dim' : 'plain');

    if (item.url) {
      rule();
      push('原文', 'head');
      wrapped(item.url, 'dim', 3);
    }
    return out;
  }

  const STYLE_COLOR = {
    head: C.fg,
    accent: C.white,
    dim: C.dim,
    bull: C.red,
    bear: C.cyan,
    plain: C.fg,
  };

  // ------------------------------------------------------------------ drawing
  function node(cls, css) {
    const el = document.createElement('div');
    if (cls) el.className = cls;
    if (css) Object.assign(el.style, css);
    screenEl.appendChild(el);
    return el;
  }

  function fit(el, maxW) {
    const w = el.offsetWidth;
    if (w > maxW && w > 0) {
      el.style.transformOrigin = 'left top';
      el.style.transform = `scaleX(${(maxW / w).toFixed(4)})`;
    }
  }

  function text(x, y, str, color, opts = {}) {
    const el = node('txt');
    el.textContent = str == null ? '' : String(str);
    el.style.color = color;
    el.style.top = y + 'px';
    if (opts.right != null) el.style.right = opts.right + 'px';
    else el.style.left = x + 'px';
    if (opts.size) { el.style.fontSize = opts.size + 'px'; el.style.lineHeight = (opts.line || opts.size + 3) + 'px'; }
    fit(el, opts.maxW != null ? opts.maxW : (opts.right != null ? W - opts.right - 4 : W - x - 2));
    return el;
  }

  function chip(x, y, str, color = C.ink, bg = C.bar) {
    const el = node('chip');
    el.textContent = str;
    el.style.color = color;
    el.style.background = bg;
    el.style.left = x + 'px';
    el.style.top = y + 'px';
    fit(el, W - x - 2);
    return el;
  }

  function rule(x, y, w, color = C.dim) {
    node('rule', { left: x + 'px', top: y + 'px', width: w + 'px', background: color });
  }

  function band(x, y, w, h, color) {
    node('band', { left: x + 'px', top: y + 'px', width: w + 'px', height: h + 'px', background: color });
  }

  // --------------------------------------------------------------------- views
  function drawStatus() {
    let banner = 'SIGNAL.LOG';
    if (state.busy) banner = 'SYNC..';
    else if (state.stale) banner = 'STALE';
    text(4, 1, '|' + banner, state.stale ? C.amber : C.fg, { maxW: 150 });
    text(0, 1, `${hhmm(clockNow())} ${state.battery}%`, C.dim, { right: 4 });
    rule(0, STATUS_H - 1, W, C.dim);
  }

  function drawFooter(hint) {
    rule(0, FOOTER_RULE, W, C.dim);
    text(4, FOOTER_Y, hint, C.dim, { maxW: W - 8 });
  }

  function drawList() {
    const items = state.feed.items;
    for (let row = 0; row < VISIBLE_ROWS && row < items.length; row++) {
      const it = items[row];
      const selected = row === state.selected;
      const y = LIST_TOP + row * ROW_H;

      if (selected) band(0, y, W, ROW_H - 2, C.bar);
      const main = selected ? C.ink : C.white;

      text(4, y + 1, `[${it.grade}] ${hhmm(it.epoch)} ${it.source}`,
           selected ? C.ink : (GRADE_COLOR[it.grade] || C.fg), { maxW: 168 });

      const lines = wrap(it.title, LIST_CELLS, 2);
      lines.forEach((line, i) => text(4, y + 16 + i * 15, line, main, { maxW: W - 8 }));
      if (!lines.length) text(4, y + 16, '(无标题)', main);

      if (it.quote.hasQuote) {
        text(0, y + 1, pctText(it.quote.changePct),
             selected ? C.ink : changeColor(it.quote), { right: 4 });
      }

      rule(0, y + ROW_H - 2, W, selected ? C.bar : C.dim);
    }

    drawFooter(`▲▼ 选择  OK 详情  ${items.length ? state.selected + 1 : 0}/${items.length}`);
  }

  function drawDetail() {
    const item = state.feed.items[state.detailIndex];
    if (!item) { state.view = 'list'; drawList(); return; }

    const lines = detailLines(item, clockNow());
    const maxFirst = Math.max(0, lines.length - DETAIL_ROWS);
    state.detailFirst = clamp(state.detailFirst, 0, maxFirst);

    band(0, STATUS_H, W, 15, C.bar);
    const head = `${state.detailIndex + 1}/${state.feed.items.length} `;
    text(4, STATUS_H, head, C.ink, { maxW: 40 });
    text(4 + head.length * 6, STATUS_H, item.source, C.ink, { maxW: W - 60 });

    for (let i = 0; i < DETAIL_ROWS; i++) {
      const line = lines[state.detailFirst + i];
      if (!line) break;
      const y = DETAIL_TOP + i * 15;
      if (line.style === 'rule') rule(4, y + 7, W - 8, C.dim);
      else if (line.style === 'head') chip(4, y + 1, line.text);
      else text(4, y, line.text, STYLE_COLOR[line.style] || C.fg, { maxW: W - 8 });
    }

    if (lines.length > state.detailFirst + DETAIL_ROWS) {
      text(W - 18, DETAIL_TOP + DETAIL_ROWS * 15 - 2, '▼', C.amber);
    }

    drawFooter('▲▼ 滚动  OK 翻页  长按 返回');
  }

  function drawMessage() {
    const isBoot = state.view === 'boot';
    const headline = isBoot ? 'SIGNAL.LOG' : 'NO SIGNAL';
    text(6, 44, headline, C.fg, { size: 20, line: 24, maxW: W - 12 });
    rule(6, 74, W - 12, C.dim);

    const body = isBoot
      ? '005 · 章鱼AI 事件雷达\n开机中，等待第一轮情报…'
      : `${state.err}\n\n检查 Wi-Fi 与 feed 地址，OK 重试。`;

    let row = 0;
    for (const paragraph of body.split('\n')) {
      const wrapped = wrap(paragraph, DETAIL_CELLS, 3);
      if (!wrapped.length) { row++; continue; }
      for (const line of wrapped) {
        if (row >= 9) break;
        text(6, 86 + row * 15, line, C.fg, { maxW: W - 12 });
        row++;
      }
    }

    if (isBoot) {
      // 启动页给一个闪烁光标，和旧终端一致
      const cursor = text(6, 86 + row * 15, '█', C.fg);
      cursor.style.animation = 'flicker 1.1s steps(2) infinite';
    }

    drawFooter(state.busy ? '正在连接…' : 'OK 重试');
  }

  function render() {
    if (!screenEl) return;
    screenEl.textContent = '';
    drawStatus();

    if (state.view === 'detail' && state.feed) drawDetail();
    else if (state.view === 'list' && state.feed) drawList();
    else drawMessage();

    updateFacts();
  }

  function updateFacts() {
    if (!factsEl) return;
    const feed = state.feed;
    document.getElementById('factFeed').textContent =
      state.busy ? '拉取中…' : `feed.json · ${state.bytes} B`;
    document.getElementById('factSync').textContent =
      feed ? `${hhmm(clockNow())}（基准 ${hhmm(feed.generatedEpoch)}）` : '—';
    document.getElementById('factCount').textContent =
      feed ? `${feed.items.length} 条 / 窗口 ${feed.windowMinutes} 分钟` : '—';
    document.getElementById('factDropped').textContent =
      feed ? `${feed.dropped} 条` : '—';
  }

  // ------------------------------------------------------------------ actions
  const nextIndex = (i, n, d) => (n <= 0 ? 0 : (i + d + n) % n);

  function openDetail(index) {
    if (!state.feed || index < 0 || index >= state.feed.items.length) return;
    state.detailIndex = index;
    state.detailFirst = 0;
    state.view = 'detail';
  }

  function act(key, kind) {
    if (kind !== 'click' && kind !== 'long') return;

    if (state.view === 'list') {
      const n = state.feed ? state.feed.items.length : 0;
      if (kind === 'click' && key === 'up') state.selected = nextIndex(state.selected, n, -1);
      else if (kind === 'click' && key === 'down') state.selected = nextIndex(state.selected, n, 1);
      else if (kind === 'click' && key === 'ok') openDetail(state.selected);
      else if (kind === 'long' && key === 'ok') { loadFeed(); return; }
    } else if (state.view === 'detail') {
      const item = state.feed && state.feed.items[state.detailIndex];
      if (!item) { state.view = 'list'; render(); return; }
      const total = detailLines(item, clockNow()).length;
      const maxFirst = Math.max(0, total - DETAIL_ROWS);
      if (kind === 'click' && key === 'up') state.detailFirst = Math.max(0, state.detailFirst - 1);
      else if (kind === 'click' && key === 'down') state.detailFirst = Math.min(maxFirst, state.detailFirst + 1);
      else if (kind === 'click' && key === 'ok') {
        state.detailFirst += DETAIL_ROWS;
        if (state.detailFirst > maxFirst) state.detailFirst = 0;
      } else if (kind === 'long' && key === 'ok') state.view = 'list';
    } else if (key === 'ok') {
      loadFeed();
      return;
    }

    render();
  }

  // -------------------------------------------------------------------- input
  document.querySelectorAll('[data-key]').forEach((btn) => {
    const key = btn.dataset.key;
    let timer = null;
    let long = false;
    let down = false;

    const press = () => {
      if (down) return;
      down = true;
      btn.classList.add('pressed');
      if (key === 'ok') {
        long = false;
        timer = setTimeout(() => { long = true; act('ok', 'long'); }, 600);
      }
    };
    const release = () => {
      if (!down) return;
      down = false;
      btn.classList.remove('pressed');
      if (key === 'ok') {
        clearTimeout(timer);
        const wasLong = long;
        long = false;
        if (!wasLong) act('ok', 'click');
      } else {
        act(key, 'click');
      }
    };

    btn.addEventListener('pointerdown', (e) => { e.preventDefault(); press(); });
    btn.addEventListener('pointerup', (e) => { e.preventDefault(); release(); });
    btn.addEventListener('pointerleave', () => { clearTimeout(timer); down = false; btn.classList.remove('pressed'); });
    btn.addEventListener('pointercancel', () => { clearTimeout(timer); down = false; btn.classList.remove('pressed'); });

    btn._press = press;
    btn._release = release;
  });

  const keyMap = { ArrowUp: 'up', ArrowDown: 'down', Enter: 'ok' };
  const isEnter = (e) => e.key === 'Enter' || e.code === 'Space';
  window.addEventListener('keydown', (e) => {
    const target = e.target;
    if (target && (target.tagName === 'BUTTON' || target.tagName === 'INPUT')) return;
    if (e.key === 'Enter') {
      e.preventDefault();
      if (e.repeat) return;
      const btn = document.querySelector('[data-key="ok"]');
      if (btn && btn._press) btn._press();
      return;
    }
    const dir = keyMap[e.key];
    if (!dir) return;
    e.preventDefault();
    act(dir, 'click');
  });
  window.addEventListener('keyup', (e) => {
    if (!isEnter(e)) return;
    const btn = document.querySelector('[data-key="ok"]');
    if (btn && btn._release) btn._release();
  });

  // control panel
  const btnRefresh = document.getElementById('btnRefresh');
  const btnOffline = document.getElementById('btnOffline');
  const btnEmpty = document.getElementById('btnEmpty');
  const btnReplay = document.getElementById('btnReplay');

  if (btnRefresh) btnRefresh.addEventListener('click', () => loadFeed());
  if (btnOffline) {
    btnOffline.addEventListener('click', () => {
      state.offline = !state.offline;
      btnOffline.setAttribute('aria-pressed', String(state.offline));
      btnOffline.textContent = state.offline ? '恢复网络' : '模拟断网';
      loadFeed();
    });
  }
  if (btnEmpty) {
    btnEmpty.addEventListener('click', () => {
      // 演示"从没拿到过数据"的分支：清掉旧 feed，再让这轮解析失败。
      state.feed = null;
      state.empty = true;
      state.view = 'boot';
      loadFeed().then(() => { state.empty = false; });
    });
  }
  if (btnReplay) {
    btnReplay.addEventListener('click', () => {
      state.view = 'boot';
      state.detailFirst = 0;
      state.stale = false;
      render();
      setTimeout(loadFeed, 700);
    });
  }

  // 顶栏时钟 / “几分钟前” 定期刷新（与固件每分钟重绘一次对应）
  let lastMinute = -1;
  setInterval(() => {
    const minute = Math.floor(clockNow() / 60000);
    if (minute !== lastMinute) {
      lastMinute = minute;
      if (state.feed) render();
    }
  }, 5000);

  // 首屏：先显示启动页，随后拉取
  render();
  setTimeout(loadFeed, 900);
})();
