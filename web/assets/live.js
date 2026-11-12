/* LAMP—1 实时监测台：数据源 + 渲染
 *
 * 三种数据源，同一套渲染：
 *   - Web Serial：浏览器直连 ESP32-S3 原生 USB（主力，零服务端）
 *   - 录制回放：assets/live-demo.json（无设备 / 不支持 Web Serial 时）
 *   - （预留给 docs/25 的 SSE 服务端中继，接口是 ingest(line)）
 */
(function () {
  'use strict';

  var P = window.LampParser;
  if (!P) return;

  var WINDOW_MS = 20000;      // 画布时间窗
  var KEEP_MS = 32000;        // 环形缓冲保留
  var DB_MIN = -70;           // 画布纵轴下限（dBFS）
  var DB_MAX = -10;
  var MAX_EVENTS = 120;

  var $ = function (id) { return document.getElementById(id); };

  var canvas = $('scope');
  var ctx = canvas.getContext('2d');

  var el = {
    badge: $('srcBadge'), srcText: $('srcText'),
    connectBtn: $('connectBtn'), disconnectBtn: $('disconnectBtn'), replayBtn: $('replayBtn'),
    scopeMeta: $('scopeMeta'), scopeEmpty: $('scopeEmpty'),
    rms: $('rmsVal'), peak: $('peakVal'), dbfs: $('dbfsVal'), vad: $('vadVal'), mn: $('mnVal'),
    port: $('linkPort'), baud: $('linkBaud'), rate: $('linkRate'), samples: $('linkSamples'),
    bytes: $('linkBytes'), errors: $('linkErrors'), resets: $('linkResets'), uptime: $('linkUptime'),
    led: $('ledState'), lastCmd: $('lastCmd'), heap: $('heapVal'), fw: $('fwVal'),
    wifiInfo: $('wifiInfo'), memInfo: $('memInfo'),
    voiceHeard: $('voiceHeard'), voiceIntent: $('voiceIntent'), voiceAction: $('voiceAction'),
    voiceReply: $('voiceReply'),
    voiceLatency: $('voiceLatency'), voiceCount: $('voiceCount'), voiceMeta: $('voiceMeta'),
    events: $('events'), eventsEmpty: $('eventsEmpty'), eventCount: $('eventCount'),
    toast: $('toast')
  };

  var ring = { mic: [], afe: [] };
  var events = [];
  var state = {
    brightness: null, mnState: null, vad: null, vadDbfs: null, vadAt: 0,
    heapKb: null, fw: null, lastCmd: null, lastUptime: 0, lastSampleAt: 0,
    lastRms: null, lastPeak: null, lastDbfs: null,
    // Arduino 固件（provider=4）用到的字段
    lastText: '', lastIntent: '', lastAction: null, asrMs: 0, voiceCount: 0,
    lastReply: '', lastVoiceAt: 0, ledPct: null, wifiIp: '', wifiRssi: null, wifiSsid: '',
    psramBytes: 0, heapBytes: 0, lastStatus: ''
  };
  var stats = {
    bytes: 0, lines: 0, samples: 0, ignored: 0, resets: 0,
    lineWin: 0, sampleWin: 0, rate: 0, sampleRate: 0, connectedAt: 0
  };

  var mode = 'idle';          // idle | connecting | live | replay | error
  var port = null;
  var reader = null;
  var readTask = null;        // 读循环的 Promise，断开时要等它收尾
  var readLoopToken = 0;
  var replayToken = 0;
  var toastTimer = 0;

  /* ---------------------------------------------------------------- 工具 */
  function clamp(v, a, b) { return v < a ? a : v > b ? b : v; }

  function fmtUptime(ms) {
    if (!ms && ms !== 0) return '—';
    var s = Math.floor(ms / 1000);
    var h = Math.floor(s / 3600);
    var m = Math.floor((s % 3600) / 60);
    var sec = s % 60;
    return (h ? h + ':' : '') + String(m).padStart(2, '0') + ':' + String(sec).padStart(2, '0');
  }

  function fmtTime(d) {
    return String(d.getHours()).padStart(2, '0') + ':' +
      String(d.getMinutes()).padStart(2, '0') + ':' +
      String(d.getSeconds()).padStart(2, '0') + '.' +
      String(Math.floor(d.getMilliseconds() / 100));
  }

  function toast(msg, ms) {
    el.toast.textContent = msg;
    el.toast.classList.add('is-on');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { el.toast.classList.remove('is-on'); }, ms || 4200);
  }

  function setStatus(next, text) {
    mode = next;
    el.badge.dataset.state = next;
    el.srcText.textContent = text;
    var live = next === 'live' || next === 'replay';
    el.disconnectBtn.hidden = !live;
    el.connectBtn.hidden = next === 'live';
    el.replayBtn.disabled = next === 'replay';
  }

  /* ------------------------------------------------------------ 数据落地 */
  function trim(now) {
    var cut = now - KEEP_MS;
    ['mic', 'afe'].forEach(function (k) {
      var arr = ring[k];
      var i = 0;
      while (i < arr.length && arr[i].t < cut) i++;
      if (i) arr.splice(0, i);
    });
  }

  function pushSample(r, hostT) {
    var arr = r.src === 'afe' ? ring.afe : ring.mic;
    arr.push({ t: hostT, db: r.dbfs });
    state.lastDbfs = r.dbfs;
    state.lastSampleAt = hostT;
    if (r.src !== 'afe') {
      if (r.rms !== undefined) state.lastRms = r.rms;
      if (r.peak !== undefined) state.lastPeak = r.peak;
    }
    trim(hostT);
  }

  function applyPatch(patch) {
    if (!patch) return;
    if (patch.brightness !== undefined) state.brightness = patch.brightness;
    if (patch.mnState !== undefined) state.mnState = patch.mnState;
    if (patch.heapKb !== undefined) state.heapKb = patch.heapKb;
    if (patch.vad !== undefined) {
      state.vad = patch.vad;
      state.vadAt = performance.now();
      if (patch.vadDbfs !== undefined) state.vadDbfs = patch.vadDbfs;
    }
  }

  function pushEvent(ev) {
    var item = {
      at: Date.now(),
      uptime: ev.uptime || 0,
      kind: ev.kind || 'system',
      text: ev.text || ''
    };
    events.unshift(item);
    if (events.length > MAX_EVENTS) events.length = MAX_EVENTS;
    if (item.kind === 'command') state.lastCmd = item.text;
    renderEvents();
  }

  var KIND_LABEL = {
    boot: '启动', wake: '唤醒', command: '命中', led: '点灯',
    button: '按键', timeout: '超时', error: '错误', system: '系统',
    trigger: '听到', listen: '录音', voice: '识别', intent: '意图',
    action: '动作', empty: '空', warn: '注意', link: '链路', reply: '回答'
  };

  function renderEvents() {
    el.eventCount.textContent = events.length ? events.length + ' 条' : '0';
    el.eventsEmpty.hidden = events.length > 0;
    var frag = document.createDocumentFragment();
    for (var i = 0; i < events.length; i++) {
      var e = events[i];
      var li = document.createElement('li');
      li.className = 'ev ev--' + e.kind;
      var t = document.createElement('time');
      var d = new Date(e.at);
      t.textContent = fmtTime(d);
      var chip = document.createElement('span');
      chip.className = 'ev-chip';
      chip.textContent = KIND_LABEL[e.kind] || e.kind;
      var body = document.createElement('span');
      body.className = 'ev-text';
      body.textContent = e.text;
      li.appendChild(t);
      li.appendChild(chip);
      li.appendChild(body);
      frag.appendChild(li);
    }
    el.events.replaceChildren(frag);
  }

  function checkReset(uptime) {
    if (!uptime) return;
    if (state.lastUptime && uptime + 1500 < state.lastUptime) {
      stats.resets++;
      ring.mic.length = 0;
      ring.afe.length = 0;
      pushEvent({ kind: 'system', text: '设备复位，计时重新开始', uptime: uptime });
      $('linkResets').textContent = String(stats.resets);
    }
    state.lastUptime = uptime;
  }

  function ingest(line, hostT) {
    if (!line) return;
    stats.lines++;
    stats.lineWin++;
    stats.bytes += line.length + 1;

    var r = P.parseLine(line);
    switch (r.type) {
      case 'sample':
        stats.samples++;
        stats.sampleWin++;
        checkReset(r.uptime);
        applyPatch(r.patch);
        pushSample(r, hostT);
        break;
      case 'state':
        checkReset(r.uptime);
        applyPatch(r.patch);
        break;
      case 'event':
        checkReset(r.uptime);
        applyPatch(r.patch);
        if (r.fw) { state.fw = r.fw; el.fw.textContent = r.fw; }
        pushEvent(r);
        break;
      case 'hello':
        state.fw = r.fw + ' · proto ' + r.proto + (r.caps.length ? ' · ' + r.caps.join('/') : '');
        el.fw.textContent = state.fw;
        pushEvent({ kind: 'system', text: '固件握手：' + state.fw, uptime: 0 });
        break;
      case 'voice':
        state.lastText = r.text || '';
        state.lastVoiceAt = hostT;
        state.voiceCount++;
        // 新一句话进来，上一句的"回答"就过期了，别让旧答案挂在新命令旁边。
        state.lastReply = '';
        pushEvent({ kind: 'voice', text: '识别「' + state.lastText + '」' });
        break;
      case 'intent':
        state.lastIntent = r.intent || '';
        pushEvent({
          kind: 'intent',
          text: (r.local ? '本地命中 → ' : '意图 ') + (r.intent || '?')
        });
        break;
      case 'reply':
        state.lastReply = r.text || '';
        state.lastVoiceAt = hostT;
        pushEvent({ kind: 'reply', text: '回答：' + state.lastReply });
        break;
      case 'action': {
        // 固件会先打 [cmd]，紧接着再打一条 {"ev":"light_set"}（同一次动作），
        // 800 ms 内只记一条事件，避免事件流里一次开灯刷两行。
        var fresh = (hostT - (state.lastActionAt || 0)) > 800;
        state.lastActionAt = hostT;
        state.lastAction = { intent: r.intent, on: r.on, pct: r.pct };
        state.ledPct = r.pct;
        state.brightness = r.pct;
        // JSON 的 light_set 只带 "light"，不要用它覆盖掉 [cmd] 里的真实意图。
        if (r.intent && r.intent !== 'light') {
          state.lastCmd = r.intent + ' → ' + (r.on ? '开' : '关') + ' ' + r.pct + '%';
        }
        if (fresh) {
          pushEvent({
            kind: 'action',
            text: (r.source ? r.source + ' · ' : '') + '灯 ' + (r.on ? '开 ' : '关 ') + r.pct + '%'
          });
        }
        break;
      }
      case 'stat':
        if (r.asrMs) state.asrMs = r.asrMs;
        break;
      case 'link':
        state.wifiSsid = r.ssid || '';
        state.wifiIp = r.ip || '';
        state.wifiRssi = r.rssi;
        pushEvent({ kind: 'link', text: 'WiFi ' + state.wifiSsid + ' · ' + state.wifiIp });
        break;
      case 'health':
        state.psramBytes = r.psram || 0;
        state.heapBytes = r.heap || 0;
        break;
      case 'status':
        state.lastStatus = r.text || '';
        break;
      default:
        stats.ignored++;
    }
  }

  /* ----------------------------------------------------------------- 串口 */
  function serialSupported() {
    return typeof navigator !== 'undefined' && 'serial' in navigator;
  }

  // 往串口写一行（固件的命令行：mic on / vad 120 / status …）
  async function sendLine(text) {
    if (!port || !port.writable) return false;
    var writer = null;
    try {
      writer = port.writable.getWriter();
      await writer.write(new TextEncoder().encode(text));
      return true;
    } catch (err) {
      toast('发送失败：' + (err && err.message ? err.message : err));
      return false;
    } finally {
      if (writer) { try { writer.releaseLock(); } catch (e) { /* ignore */ } }
    }
  }

  async function connect() {
    if (mode === 'connecting' || mode === 'live') return;
    replayToken++;                     // 连真机时停掉回放
    if (!serialSupported()) {
      toast('此浏览器不支持 Web Serial：请用 Chrome / Edge 桌面版，或点「播放录制」。');
      return;
    }
    var chosen;
    try {
      chosen = await navigator.serial.requestPort();
    } catch (err) {
      if (err && err.name === 'NotFoundError') return;   // 用户取消
      toast('选择端口失败：' + (err && err.message ? err.message : err));
      return;
    }
    await openPort(chosen);
  }

  async function openPort(p) {
    setStatus('connecting', '连接中');
    el.scopeMeta.textContent = '正在打开串口…';
    try {
      await p.open({ baudRate: 115200, bufferSize: 8192 });
    } catch (err) {
      var name = err && err.name;
      if (name === 'NetworkError' || name === 'InvalidStateError' || name === 'NotFoundError') {
        toast('端口打不开：多半被别的程序占着。先关掉 idf.py monitor / 串口助手再试。', 6000);
      } else if (name === 'SecurityError') {
        toast('需要 https 或 localhost：file:// 打开时 Web Serial 被禁用。', 6000);
      } else {
        toast('打开失败：' + (err && err.message ? err.message : err), 6000);
      }
      setStatus('error', '连接失败');
      el.scopeMeta.textContent = '连接失败';
      return;
    }

    port = p;
    stats.connectedAt = performance.now();
    stats.bytes = 0; stats.lines = 0; stats.samples = 0; stats.ignored = 0;
    stats.rate = 0; stats.sampleRate = 0; stats.lineWin = 0; stats.sampleWin = 0;
    ring.mic.length = 0; ring.afe.length = 0;
    state.lastUptime = 0;
    var info = (p.getInfo && p.getInfo()) || {};
    el.port.textContent = info.usbVendorId === 0x303a
      ? 'ESP32-S3 原生 USB'
      : (info.usbVendorId ? 'USB ' + info.usbVendorId.toString(16) : '已选端口');

    setStatus('live', '已连接');
    el.scopeMeta.textContent = '实时 · 115200 · 等待数据';
    pushEvent({ kind: 'system', text: '串口已连接', uptime: 0 });
    readTask = readPort(++readLoopToken);
    // 这版固件平时不打印电平（只有 mic monitor 打开时才打），连上后自动开一下：
    // 曲线才有数据，用户也能立刻看到麦克风是不是在工作。
    setTimeout(function () {
      if (mode === 'live') {
        sendLine('mic on\n').then(function (ok) {
          if (ok) pushEvent({ kind: 'system', text: '已发送 mic on（开启电平显示）', uptime: 0 });
        });
      }
    }, 1800);
  }

  async function readPort(token) {
    if (!port || !port.readable) return;
    var decoder = new TextDecoderStream();
    var pipeDone = port.readable.pipeTo(decoder.writable).catch(function () {});
    reader = decoder.readable.getReader();
    var buf = '';
    try {
      while (token === readLoopToken) {
        var chunk = await reader.read();
        if (chunk.done) break;
        buf += chunk.value;
        var idx;
        while ((idx = buf.indexOf('\n')) >= 0) {
          var line = buf.slice(0, idx).replace(/\r$/, '');
          buf = buf.slice(idx + 1);
          if (line) ingest(line, performance.now());
        }
        if (buf.length > 65536) buf = '';   // 二进制垃圾时的保护
      }
    } catch (err) {
      if (!(err && err.name === 'NetworkError')) {
        toast('读取中断：' + (err && err.message ? err.message : err));
      }
    } finally {
      try { reader.releaseLock(); } catch (e) { /* 已释放 */ }
      try { await pipeDone; } catch (e) { /* 已断开 */ }
      if (token === readLoopToken) {
        port = null;
        setStatus('error', '已断开');
        el.scopeMeta.textContent = '已断开 · 可重新连接';
        pushEvent({ kind: 'system', text: '串口断开', uptime: 0 });
      }
    }
  }

  async function disconnect(silent) {
    readLoopToken++;
    try { if (reader) await reader.cancel(); } catch (e) { /* ignore */ }
    if (readTask) {
      try { await readTask; } catch (e) { /* ignore */ }
      readTask = null;
    }
    reader = null;
    var p = port;
    port = null;
    if (p) {
      try {
        await p.close();
      } catch (e) {
        // 读循环还没完全释放时 close() 会抛；等一下再试一次
        await sleep(60);
        try { await p.close(); } catch (e2) { /* 交给下次连接 */ }
      }
    }
    if (!silent) {
      setStatus('idle', '未连接');
      el.scopeMeta.textContent = '等待连接';
    }
  }

  /* ------------------------------------------------------------ 录制回放 */
  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  async function playReplay() {
    var token = ++replayToken;
    await disconnect(true);
    setStatus('replay', '录制回放');
    ring.mic.length = 0; ring.afe.length = 0;
    events.length = 0;
    stats.bytes = 0; stats.lines = 0; stats.samples = 0; stats.ignored = 0;
    stats.lineWin = 0; stats.sampleWin = 0;
    renderEvents();
    el.scopeMeta.textContent = '加载录制…';

    var data;
    try {
      var res = await fetch('assets/live-demo.json', { cache: 'no-store' });
      data = await res.json();
    } catch (err) {
      toast('回放文件读不到：' + (err && err.message ? err.message : err));
      setStatus('idle', '未连接');
      return;
    }
    var stamp = (data.capturedAt || '').replace('T', ' ').slice(0, 16);
    el.scopeMeta.textContent = '录制回放 · ' + stamp + ' · 1×';
    pushEvent({ kind: 'system', text: '开始回放录制（' + (data.port || '') + ' · ' + data.lines.length + ' 行）', uptime: 0 });

    var prev = 0;
    for (var i = 0; i < data.lines.length; i++) {
      var item = data.lines[i];
      var wait = Math.max(0, item[0] - prev);
      prev = item[0];
      await sleep(Math.min(wait, 2400));
      if (token !== replayToken) return;
      ingest(item[1], performance.now());
    }
    if (token === replayToken) {
      el.scopeMeta.textContent = '回放结束 · ' + stamp + ' · 可点「播放录制」重放';
      pushEvent({ kind: 'system', text: '回放结束', uptime: 0 });
    }
  }

  /* -------------------------------------------------------------- 渲染 */
  function renderReadout(now) {
    var fresh = now - state.lastSampleAt < 6000;
    el.rms.textContent = fresh && state.lastRms !== null ? String(state.lastRms) : '—';
    el.peak.textContent = fresh && state.lastPeak !== null ? String(state.lastPeak) : '—';
    el.dbfs.textContent = fresh && state.lastDbfs !== null ? state.lastDbfs.toFixed(1) + ' dB' : '—';

    var vadFresh = state.vad !== null && now - state.vadAt < 4000;
    el.vad.textContent = vadFresh ? (state.vad ? '语音' : '静音') : '—';
    el.vad.dataset.on = vadFresh ? String(state.vad) : 'unknown';

    var mnLabel = { 0: '监听中', 1: '命中', 2: '窗口过期' };
    el.mn.textContent = state.mnState === null ? '—' : (mnLabel[state.mnState] || String(state.mnState));
    el.mn.dataset.on = state.mnState === 1 ? 'hit' : state.mnState === 2 ? 'stale' : 'idle';

    el.led.textContent = state.brightness === null ? '—' :
      (state.brightness > 0 ? state.brightness + ' · ON' : '0 · OFF');
    el.lastCmd.textContent = state.lastCmd || '—';
    el.heap.textContent = state.heapKb !== null
      ? state.heapKb + ' KB'
      : (state.heapBytes ? Math.round(state.heapBytes / 1024) + ' KB' : '—（固件未上报）');

    // ---- 语音卡（firmware/ 那版固件：ASR → LLM → 动作）----
    if (el.voiceHeard) {
      el.voiceHeard.textContent = state.lastText || '等待说话…';
      el.voiceHeard.classList.toggle('is-empty', !state.lastText);
      el.voiceIntent.textContent = state.lastIntent || '—';
      el.voiceAction.textContent = state.lastAction
        ? (state.lastAction.on ? '开 ' : '关 ') + state.lastAction.pct + '%'
        : '—';
      el.voiceLatency.textContent = state.asrMs ? state.asrMs + ' ms' : '—';
      el.voiceCount.textContent = String(state.voiceCount || 0);
      if (el.voiceReply) {
        el.voiceReply.hidden = !state.lastReply;
        el.voiceReply.textContent = state.lastReply ? '回答：' + state.lastReply : '';
      }
      if (state.lastVoiceAt) {
        var age = Math.max(0, now - state.lastVoiceAt);
        el.voiceMeta.textContent = age < 6000 ? '刚刚识别' : Math.round(age / 1000) + ' 秒前';
      } else {
        el.voiceMeta.textContent = '等待说话';
      }
    }

    if (el.wifiInfo) {
      el.wifiInfo.textContent = state.wifiIp
        ? state.wifiIp + (state.wifiRssi !== null ? ' · ' + state.wifiRssi + ' dBm' : '')
        : '—';
    }
    if (el.memInfo) {
      el.memInfo.textContent = state.psramBytes
        ? (state.psramBytes / 1048576).toFixed(1) + ' MB · heap ' +
          Math.round(state.heapBytes / 1024) + ' KB'
        : '—';
    }
  }

  function renderStats(now) {
    stats.rate = stats.lineWin;
    stats.sampleRate = stats.sampleWin;
    stats.lineWin = 0;
    stats.sampleWin = 0;
    el.rate.textContent = mode === 'idle' ? '—' : stats.rate + ' 行/秒';
    el.samples.textContent = mode === 'idle' ? '—' : stats.sampleRate + ' /秒';
    el.bytes.textContent = stats.bytes > 1024 ? (stats.bytes / 1024).toFixed(1) + ' KB' : stats.bytes + ' B';
    el.errors.textContent = String(stats.ignored);
    el.resets.textContent = String(stats.resets);
    el.uptime.textContent = state.lastUptime ? fmtUptime(state.lastUptime) : '—';
    renderReadout(now);
  }

  function drawSeries(arr, now, x0, x1, y0, y1, stroke, width, glow) {
    if (arr.length < 2) return;
    var xOf = function (t) { return x1 - (now - t) / WINDOW_MS * (x1 - x0); };
    var yOf = function (db) {
      var c = clamp(db, DB_MIN, DB_MAX);
      return y1 - (c - DB_MIN) / (DB_MAX - DB_MIN) * (y1 - y0);
    };
    ctx.save();
    ctx.strokeStyle = stroke;
    ctx.lineWidth = width;
    ctx.lineJoin = 'round';
    ctx.lineCap = 'round';
    if (glow) { ctx.shadowColor = stroke; ctx.shadowBlur = 5; }
    ctx.beginPath();
    var started = false;
    var prevX = null;
    for (var i = 0; i < arr.length; i++) {
      var p = arr[i];
      var x = xOf(p.t);
      if (x < x0 - 4) continue;
      if (prevX !== null && x - prevX > 60) started = false;   // 数据断档就断开折线
      var y = yOf(p.db);
      if (!started) { ctx.moveTo(x, y); started = true; } else { ctx.lineTo(x, y); }
      prevX = x;
    }
    ctx.stroke();
    ctx.restore();
  }

  function drawScope(now) {
    var w = canvas.clientWidth;
    var h = canvas.clientHeight;
    if (!w || !h) return;
    var dpr = Math.min(2, window.devicePixelRatio || 1);
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    var x0 = 44, x1 = w - 12, y0 = 14, y1 = h - 20;

    ctx.font = '10px "Cascadia Mono", Consolas, monospace';
    ctx.textBaseline = 'middle';

    // 横向刻度：每 20 dB 一条
    for (var db = DB_MAX; db >= DB_MIN; db -= 20) {
      var y = y1 - (db - DB_MIN) / (DB_MAX - DB_MIN) * (y1 - y0);
      ctx.strokeStyle = 'rgba(242,237,226,.07)';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(x0, y + 0.5);
      ctx.lineTo(x1, y + 0.5);
      ctx.stroke();
      ctx.fillStyle = 'rgba(143,146,134,.85)';
      ctx.textAlign = 'right';
      ctx.fillText(String(db), x0 - 8, y);
    }

    // 纵向刻度：20 / 10 / 0 秒
    ctx.textAlign = 'center';
    [20, 10, 0].forEach(function (s) {
      var x = x1 - s / 20 * (x1 - x0);
      ctx.strokeStyle = 'rgba(242,237,226,.05)';
      ctx.beginPath();
      ctx.moveTo(x + 0.5, y0);
      ctx.lineTo(x + 0.5, y1);
      ctx.stroke();
      ctx.fillStyle = 'rgba(107,112,100,.9)';
      ctx.fillText(s === 0 ? '现在' : '-' + s + 's', x, y1 + 10);
    });

    drawSeries(ring.afe, now, x0, x1, y0, y1, 'rgba(182,203,168,.55)', 1.1, false);
    drawSeries(ring.mic, now, x0, x1, y0, y1, '#e5b776', 1.9, true);

    // 图例
    ctx.textAlign = 'left';
    ctx.fillStyle = '#e5b776';
    ctx.fillText('■ RMS（麦克风）', x0 + 4, y0 + 8);
    ctx.fillStyle = 'rgba(182,203,168,.8)';
    ctx.fillText('■ AFE 音量', x0 + 108, y0 + 8);

    var empty = ring.mic.length === 0 && ring.afe.length === 0;
    el.scopeEmpty.hidden = !empty;
  }

  /* ------------------------------------------------------------ 主循环 */
  var lastTick = 0;
  function frame(now) {
    if (now - lastTick > 1000) {
      lastTick = now;
      renderStats(now);
    }
    drawScope(now);
    requestAnimationFrame(frame);
  }

  /* --------------------------------------------------------------- 启动 */
  // 调试/测试钩子：Console 里可以直接 __lampLive.ingest('...') 灌一行假数据
  window.__lampLive = { ingest: ingest, state: state, stats: stats, ring: ring, events: events };

  function boot() {
    if (!serialSupported()) {
      el.connectBtn.disabled = true;
      el.connectBtn.textContent = '浏览器不支持';
      toast('Web Serial 只在 Chrome / Edge 桌面版可用。可以先用「播放录制」看真实数据。', 8000);
    }
    el.connectBtn.addEventListener('click', connect);
    el.disconnectBtn.addEventListener('click', function () { disconnect(false); });
    el.replayBtn.addEventListener('click', playReplay);

    var cmdForm = $('cmdForm');
    if (cmdForm) {
      cmdForm.addEventListener('submit', function (ev) {
        ev.preventDefault();
        var input = $('cmdInput');
        var text = (input.value || '').trim();
        if (!text) return;
        if (mode !== 'live') { toast('先连接串口再发命令'); return; }
        sendLine(text + '\n').then(function (ok) {
          if (ok) {
            input.value = '';
            toast('已发送：' + text);
          }
        });
      });
    }

    if ('serial' in navigator) {
      navigator.serial.addEventListener('disconnect', function () {
        if (mode === 'live') {
          toast('设备已拔出');
          readLoopToken++;
          port = null;
          setStatus('error', '设备已拔出');
        }
      });
    }

    if (new URLSearchParams(location.search).get('auto') === '1') playReplay();

    requestAnimationFrame(frame);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})();
