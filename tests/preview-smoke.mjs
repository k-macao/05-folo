// tests/preview-smoke.mjs —— 在 jsdom 里跑 preview/app.js，验证预览真的能渲染与响应按键。
//
//   npm install --no-save jsdom
//   node tests/preview-smoke.mjs
//
// 覆盖：启动页 → 列表 → 选中移动 → 详情 → 长按返回 → 断网 STALE → 空数据 NO SIGNAL。
import fs from 'node:fs';
import path from 'node:path';
import { JSDOM, VirtualConsole } from 'jsdom';

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const dir = path.join(root, 'preview');
const html = fs.readFileSync(path.join(dir, 'index.html'), 'utf8');
const app = fs.readFileSync(path.join(dir, 'app.js'), 'utf8');
const feed = fs.readFileSync(path.join(dir, 'feed.json'), 'utf8');

const errors = [];
const vc = new VirtualConsole();
vc.on('jsdomError', (e) => errors.push('jsdomError: ' + e.message));
vc.on('error', (...args) => errors.push('console.error: ' + args.join(' ')));

const dom = new JSDOM(
  html.replace('<script src="app.js"></script>', `<script>${app}</script>`),
  { runScripts: 'dangerously', pretendToBeVisual: true, url: 'https://example.test/preview/', virtualConsole: vc },
);
const { window } = dom;
window.fetch = async () => ({ ok: true, status: 200, text: async () => feed });

const screen = window.document.getElementById('screen');
const text = () => screen.textContent;
const wait = (ms) => new Promise((r) => setTimeout(r, ms));
const key = (k, type = 'keydown') => window.dispatchEvent(new window.KeyboardEvent(type, { key: k, bubbles: true }));
const click = (el) => el.dispatchEvent(new window.MouseEvent('click', { bubbles: true }));

let failures = 0;
function check(cond, msg) {
  console.log((cond ? 'ok   ' : 'FAIL ') + msg);
  if (!cond) failures++;
}

await wait(60);
check(text().includes('SIGNAL.LOG'), 'boot: status bar shows SIGNAL.LOG');
check(text().includes('章鱼AI'), 'boot: message mentions 005');

await wait(1600);
check(text().includes('[EXACT]'), 'list: grade badges rendered');
check(text().includes('国常会'), 'list: first title rendered');
check(/1\/8/.test(text()), 'list: footer shows 1/8');
check(screen.querySelectorAll('.band').length === 1, 'list: exactly one selected row band');

key('ArrowDown');
key('ArrowUp', 'keyup');
await wait(20);
check(/2\/8/.test(text()), 'list: ArrowDown moves selection to 2/8');

key('Enter');
key('Enter', 'keyup');
await wait(20);
check(text().includes('AI 一句话'), 'detail: Enter opens detail');
check(text().includes('北方稀土'), 'detail: quote line present');
check(screen.querySelectorAll('.chip').length >= 1, 'detail: reverse-video chips drawn');

key('Enter');
await wait(700);
key('Enter', 'keyup');
await wait(20);
check(/▲▼ 选择/.test(text()), 'detail: long press returns to list');

const offline = window.document.getElementById('btnOffline');
click(offline);
await wait(1600);
check(text().includes('STALE'), 'offline: status shows STALE');
check(text().includes('[EXACT]'), 'offline: previous items stay on screen');

click(offline);
await wait(1200);

click(window.document.getElementById('btnEmpty'));
await wait(2200);
check(text().includes('NO SIGNAL'), 'empty: NO SIGNAL when nothing usable ever arrived');

check(errors.length === 0, 'no runtime errors' + (errors.length ? ': ' + errors.join(' | ') : ''));

process.exit(failures ? 1 : 0);
