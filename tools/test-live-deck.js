/* 用最小 DOM 桩在 Node 里跑 web/assets/live.js，验证：
 *   1) 录制数据灌进去不抛异常
 *   2) 状态/事件/采样计数正确
 *   3) 命令词 → LED 的链路能落到 state 和事件流
 *   4) #T / #H 结构化信道可用
 */
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const ROOT = path.resolve(__dirname, '..');

function makeCtx() {
  const noop = () => {};
  return new Proxy({
    setTransform: noop, clearRect: noop, beginPath: noop, moveTo: noop, lineTo: noop,
    stroke: noop, fillText: noop, save: noop, restore: noop, closePath: noop, fill: noop,
    measureText: () => ({ width: 20 })
  }, { get: (t, k) => (k in t ? t[k] : undefined), set: () => true });
}

function makeEl(id) {
  const el = {
    id,
    textContent: '',
    hidden: false,
    disabled: false,
    dataset: {},
    style: {},
    clientWidth: 640,
    clientHeight: 236,
    width: 0,
    height: 0,
    classList: { add() {}, remove() {}, toggle() {}, contains: () => false },
    appendChild() {},
    replaceChildren() {},
    addEventListener() {},
    removeEventListener() {},
    setAttribute() {},
    getContext: () => makeCtx(),
    getInfo: () => ({ usbVendorId: 0x303a })
  };
  return el;
}

const els = new Map();
const document = {
  readyState: 'complete',
  getElementById(id) {
    if (!els.has(id)) els.set(id, makeEl(id));
    return els.get(id);
  },
  createElement: (tag) => makeEl(tag),
  createDocumentFragment: () => makeEl('fragment'),
  addEventListener() {}
};

const sandbox = {
  console,
  document,
  location: { search: '' },
  navigator: {},
  performance: { now: () => Date.now() },
  requestAnimationFrame: () => 0,
  setTimeout, clearTimeout,
  URLSearchParams,
  TextDecoderStream: class {},
  fetch: async () => ({ json: async () => ({}) })
};
sandbox.window = sandbox;
sandbox.globalThis = sandbox;

const ctx = vm.createContext(sandbox);
for (const f of ['assets/live-parser.js', 'assets/live.js']) {
  vm.runInContext(fs.readFileSync(path.join(ROOT, 'web', f), 'utf8'), ctx, { filename: f });
}

const live = sandbox.__lampLive;
if (!live) throw new Error('__lampLive 未挂载');

let failures = 0;
function check(name, cond, extra) {
  if (cond) {
    console.log('  ok   ' + name);
  } else {
    failures++;
    console.log('  FAIL ' + name + (extra ? ' -> ' + extra : ''));
  }
}

/* ---- 1. 录制回放：不抛异常、能出采样 ---- */
const demo = JSON.parse(fs.readFileSync(path.join(ROOT, 'web/assets/live-demo.json'), 'utf8'));
let t = 1000;
for (const [dt, line] of demo.lines) {
  t += 40;
  live.ingest(line, t);
}
console.log('\n[录制数据 %d 行]', demo.lines.length);
check('麦克风采样进入环形缓冲', live.ring.mic.length > 5, 'mic=' + live.ring.mic.length);
check('AFE 采样进入环形缓冲', live.ring.afe.length > 5, 'afe=' + live.ring.afe.length);
check('没有把采样记成事件', live.events.length === 0, 'events=' + live.events.length);
check('VAD 状态被记录', live.state.vad !== null, 'vad=' + live.state.vad);
check('识别窗口状态被记录', live.state.mnState !== null, 'mn=' + live.state.mnState);
check('行计数 > 采样计数', live.stats.lines > live.stats.samples,
  'lines=' + live.stats.lines + ' samples=' + live.stats.samples);

/* ---- 2. 命令词 → 事件 + LED 状态 ---- */
const before = live.events.length;
live.ingest('W (2123456) lamp_sr: MN id=1 prob=0.870 text=da kai deng', t + 10);
live.ingest('W (2123460) lamp_sr: >>> voice:on level=70 LED=ON', t + 20);
console.log('\n[合成命令]');
check('新增 2 条事件', live.events.length === before + 2, 'got ' + (live.events.length - before));
check('事件是倒序插入（最新在前）', live.events[0].kind === 'led', 'kind=' + live.events[0].kind);
check('命中事件带中文命令名与概率', /开灯/.test(live.events[1].text) && /0\.870/.test(live.events[1].text), live.events[1].text);
check('LED 亮度落到 state', live.state.brightness === 70, 'brightness=' + live.state.brightness);
check('最近命中被记录', /开灯/.test(live.state.lastCmd || ''), live.state.lastCmd);

/* ---- 3. 结构化信道 #T / #S / #E / #H ---- */
live.ingest('#H,{"fw":"0.5.0","proto":1,"caps":["led","mic"]}', t + 30);
console.log('\n[结构化信道]');
check('#H 握手写入固件信息', /0\.5\.0/.test(live.state.fw || ''), live.state.fw);

const micBefore = live.ring.mic.length;
live.ingest('#T,2123500,220,1500,-43.2,1,0,122,70', t + 40);
check('#T 生成一条 dev 采样', live.ring.mic.length === micBefore + 1, 'mic=' + live.ring.mic.length);
check('#T 带上堆信息', live.state.heapKb === 122, 'heap=' + live.state.heapKb);

live.ingest('#E,2123510,command,命中「关灯」p=0.91', t + 50);
check('#E 事件入流', live.events[0].kind === 'command', 'kind=' + live.events[0].kind);

live.ingest('#S,2123520,brightness=0,heap=118', t + 60);
check('#S 状态更新', live.state.brightness === 0 && live.state.heapKb === 118,
  'bri=' + live.state.brightness + ' heap=' + live.state.heapKb);

/* ---- 4. 复位识别 ---- */
live.ingest('I (3500) lamp_sr: LAMP-SR ready (window=600000ms, auto-reopen)', 2000);
console.log('\n[复位]');
check('uptime 回退被记为复位', live.stats.resets >= 1, 'resets=' + live.stats.resets);
check('复位后清空曲线', live.ring.mic.length === 0 && live.ring.afe.length === 0,
  'mic=' + live.ring.mic.length + ' afe=' + live.ring.afe.length);
check('复位后有系统事件', live.events.some(e => e.kind === 'system'), '');

/* ---- 5. Arduino 固件的连接类日志（provider=4）---- */
console.log('\n[连接类日志]');
live.ingest('[wifi] status=6 WL_DISCONNECTED', t + 70);
check('WiFi 重连失败记成事件', live.events[0].kind === 'warn',
  'kind=' + live.events[0].kind);
check('事件带状态码', live.state.wifiStatus === 6, 'wifiStatus=' + live.state.wifiStatus);
check('断线后清掉旧 IP', live.state.wifiIp === '', 'ip=' + live.state.wifiIp);

live.ingest('[wifi] scan: 3 AP(s) visible', t + 80);
check('扫到 AP 记成事件', /3 个 AP/.test(live.events[0].text), live.events[0].text);

live.ingest('[wifi]   -62 dBm ch6 Xiaomi_037C  <== match', t + 90);
check('目标 SSID 命中被标出来', /SSID 命中/.test(live.events[0].text), live.events[0].text);

live.ingest('[wifi] connected ssid="Xiaomi_037C" ip=192.168.31.91 rssi=-28', t + 100);
check('连上后状态码归位', live.state.wifiStatus === 3, 'wifiStatus=' + live.state.wifiStatus);
check('连上后记录 IP', live.state.wifiIp === '192.168.31.91', live.state.wifiIp);

live.ingest('[voice] cloud=tencent wifi=down mic=ready state=idle monitor=1', t + 110);
check('status 输出可见', live.events[0].text.indexOf('cloud=tencent') >= 0, live.events[0].text);

console.log('\n' + (failures ? failures + ' 项失败' : '全部通过'));
process.exit(failures ? 1 : 0);
