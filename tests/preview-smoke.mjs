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
// 固定夹具：每日新闻会换内容，冒烟测试不能依赖当天的 feed.json。
// 想用当天真实 feed 跑一遍：FEED_PATH=preview/feed.json node tests/preview-smoke.mjs
const feedPath = process.env.FEED_PATH
  ? path.resolve(root, process.env.FEED_PATH)
  : path.join(root, 'tests/fixtures/preview-feed.json');
const feed = fs.readFileSync(feedPath, 'utf8');
const feedObj = JSON.parse(feed);
const total = Math.min(feedObj.items.length, 12);
const second = feedObj.items[1] || feedObj.items[0];

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
check(text().includes(second.title.slice(0, 6)), 'list: titles rendered');
check(new RegExp(`1/${total}`).test(text()), `list: footer shows 1/${total}`);
check(screen.querySelectorAll('.band').length === 1, 'list: exactly one selected row band');

key('ArrowDown');
key('ArrowUp', 'keyup');
await wait(20);
check(new RegExp(`2/${total}`).test(text()), `list: ArrowDown moves selection to 2/${total}`);

key('Enter');
key('Enter', 'keyup');
await wait(20);
check(text().includes('摘要'), 'detail: Enter opens detail');
check(text().includes('来源'), 'detail: source line present');
check(screen.querySelectorAll('.chip').length >= 1, 'detail: reverse-video chips drawn');
check(!text().includes('http'), 'detail: no original link on screen');

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
