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
    wifiBtn: $('wifiBtn'), wifiDialog: $('wifiDialog'), wifiCloseBtn: $('wifiCloseBtn'),
    wifiForm: $('wifiForm'), wifiSsid: $('wifiSsid'), wifiPass: $('wifiPass'),
    wifiPassToggle: $('wifiPassToggle'), wifiConnectBtn: $('wifiConnectBtn'),
    wifiClearBtn: $('wifiClearBtn'), wifiScanBtn: $('wifiScanBtn'),
    wifiScanList: $('wifiScanList'), wifiScanNote: $('wifiScanNote'),
    wifiDialogStatus: $('wifiDialogStatus'), wifiHint: $('wifiHint'),
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
    wifiStatus: null,
    // 网页配网面板：凭据只经过串口，不经过服务器
    wifiConfigured: false, wifiPasswordSet: false,
    wifiConnecting: false, wifiSupported: null, wifiLastReason: '',
    wifiScan: { phase: 'idle', expected: -1, items: [], error: '' },
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
  var wifiScanTimer = 0;
  var wifiConnectTimer = 0;
  var wifiProbeTimer = 0;

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

  var WIFI_FIELD_LABEL = { ssid: 'SSID', password: '密码', nvs: 'NVS' };

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
        // 重连失败时固件每 10 s 报一次状态码：断线就清掉旧 IP，
        // 否则顶栏会一直显示上次连上的地址，看着像「网是好的」。
        if (r.wifiStatus !== undefined) {
          state.wifiSupported = true;
          state.wifiStatus = r.wifiStatus;
          if (r.wifiStatus !== 3) {
            state.wifiIp = '';
            state.wifiRssi = null;
            state.wifiConnecting = false;
            state.wifiLastReason = r.text || ('WiFi 状态 ' + r.wifiStatus);
            clearWifiConnectTimer();
          }
          renderWifiStatus();
        }
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
        state.wifiSupported = true;
        state.wifiConfigured = true;
        state.wifiSsid = r.ssid || '';
        state.wifiIp = r.ip || '';
        state.wifiRssi = r.rssi;
        state.wifiStatus = 3;
        state.wifiConnecting = false;
        state.wifiLastReason = '';
        clearWifiConnectTimer();
        pushEvent({ kind: 'link', text: 'WiFi ' + state.wifiSsid + ' · ' + state.wifiIp });
        renderWifiStatus();
        break;
      case 'wifiSaved':
        state.wifiSupported = true;
        state.wifiConfigured = true;
        state.wifiSsid = r.ssid || '';
        state.wifiPasswordSet = !!r.hasPassword;
        state.wifiConnecting = true;
        state.wifiLastReason = '';
        pushEvent({ kind: 'link', text: '已保存 WiFi 凭据：' + state.wifiSsid + '，正在连接…' });
        renderWifiStatus();
        break;
      case 'wifiConnecting':
        state.wifiSupported = true;
        state.wifiConfigured = true;
        if (r.ssid) state.wifiSsid = r.ssid;
        state.wifiConnecting = true;
        renderWifiStatus();
        break;
      case 'wifiCleared':
        state.wifiSupported = true;
        state.wifiConfigured = false;
        state.wifiSsid = '';
        state.wifiIp = '';
        state.wifiRssi = null;
        state.wifiStatus = null;
        state.wifiConnecting = false;
        state.wifiPasswordSet = false;
        state.wifiLastReason = '已清除设备里保存的 WiFi 凭据。';
        clearWifiTimers();
        pushEvent({ kind: 'warn', text: '已清除设备里保存的 WiFi 凭据' });
        renderWifiStatus();
        break;
      case 'wifiCred':
        state.wifiSupported = true;
        state.wifiConfigured = true;
        state.wifiSsid = r.ssid || '';
        state.wifiPasswordSet = !!r.hasPassword;
        renderWifiStatus();
        break;
      case 'wifiCredNone':
        state.wifiSupported = true;
        state.wifiConfigured = false;
        state.wifiSsid = '';
        state.wifiPasswordSet = false;
        renderWifiStatus();
        break;
      case 'wifiError':
        state.wifiSupported = true;
        state.wifiConnecting = false;
        state.wifiLastReason = '设备拒绝：' + (WIFI_FIELD_LABEL[r.field] || r.field) +
          (r.detail ? ' ' + r.detail : '');
        clearWifiConnectTimer();
        pushEvent({ kind: 'warn', text: 'WiFi 配置失败：' + (r.detail || r.field) });
        renderWifiStatus();
        break;
      case 'wifiScan':
        state.wifiSupported = true;
        state.wifiScan.phase = 'scanning';
        state.wifiScan.expected = r.count;
        state.wifiScan.items = [];
        state.wifiScan.error = '';
        startWifiScanTimer();
        pushEvent({ kind: 'system', text: 'WiFi 扫描：看到 ' + r.count + ' 个网络' });
        renderWifiScan();
        break;
      case 'wifiAp':
        if (r.ssid) addWifiAp(r);
        break;
      case 'wifiScanDone':
        state.wifiScan.phase = 'done';
        clearWifiScanTimer();
        renderWifiScan();
        break;
      case 'wifiScanFailed':
        state.wifiScan.phase = 'error';
        state.wifiScan.error = '扫描失败（code ' + r.code + '），射频可能正忙；等 2 秒再试。';
        clearWifiScanTimer();
        pushEvent({ kind: 'warn', text: 'WiFi 扫描失败 code=' + r.code });
        renderWifiScan();
        break;
      case 'unknownCommand':
        if (r.command === 'wifi') {
          state.wifiSupported = false;
          state.wifiConnecting = false;
          state.wifiLastReason = '当前固件没有 wifi 命令：请先烧录最新 firmware/ 再试。';
          clearWifiTimers();
          pushEvent({ kind: 'warn', text: '固件不认识 wifi 命令（需重新烧录固件）' });
        } else {
          pushEvent({ kind: 'warn', text: '固件不认识命令：' + r.command });
        }
        renderWifiStatus();
        break;
      case 'health':
        state.psramBytes = r.psram || 0;
        state.heapBytes = r.heap || 0;
        break;
      case 'status': {
        state.lastStatus = r.text || '';
        // 以前只存不进事件流，于是在页面里敲 status 什么都看不到——
        // 而它恰好是判断 wifi / key / clock 的唯一入口。现在照实落一行。
        var wm = /^wifi=(up|down|unconfigured)\b(.*)$/.exec(state.lastStatus);
        if (wm) {
          state.wifiSupported = true;
          if (wm[1] === 'up') {
            var sm = /ssid="([^"]*)"/.exec(wm[2]);
            var im = /ip=(\S+)/.exec(wm[2]);
            if (sm) state.wifiSsid = sm[1];
            if (im) state.wifiIp = im[1];
            state.wifiConfigured = true;
            state.wifiStatus = 3;
            state.wifiConnecting = false;
            state.wifiLastReason = '';
          } else if (wm[1] === 'unconfigured') {
            state.wifiConfigured = false;
            state.wifiSsid = '';
            state.wifiIp = '';
          } else {
            state.wifiIp = '';
            state.wifiConnecting = false;
          }
          renderWifiStatus();
        }
        pushEvent({ kind: 'system', text: r.text || '' });
        break;
      }
      default:
        stats.ignored++;
    }
  }

  /* ------------------------------------------------------------ WiFi 面板 */
  // 凭据只在「设备 ↔ 浏览器」之间走串口：网页把 SSID/密码用 encodeURIComponent
  // 编码成命令行参数，固件解码后写进自己的 NVS，密码永远不会回传。
  function utf8Length(text) {
    var n = 0;
    for (var i = 0; i < text.length; i++) {
      var c = text.charCodeAt(i);
      if (c < 0x80) n += 1;
      else if (c < 0x800) n += 2;
      else if (c >= 0xd800 && c <= 0xdbff) { n += 4; i++; }
      else n += 3;
    }
    return n;
  }

  // 固件用单独的 "-" 表示开放网络（空密码）；密码本身是 "-" 时必须转义成 %2D。
  function commandSafePassword(pass) {
    var encoded = encodeURIComponent(pass);
    return encoded === '-' ? '%2D' : encoded;
  }

  function clearWifiScanTimer() {
    if (wifiScanTimer) { clearTimeout(wifiScanTimer); wifiScanTimer = 0; }
  }

  function clearWifiConnectTimer() {
    if (wifiConnectTimer) { clearTimeout(wifiConnectTimer); wifiConnectTimer = 0; }
  }

  function clearWifiTimers() {
    clearWifiScanTimer();
    clearWifiConnectTimer();
    if (wifiProbeTimer) { clearTimeout(wifiProbeTimer); wifiProbeTimer = 0; }
  }

  function startWifiScanTimer() {
    clearWifiScanTimer();
    wifiScanTimer = setTimeout(function () {
      wifiScanTimer = 0;
      if (state.wifiScan.phase !== 'scanning') return;
      state.wifiScan.phase = 'error';
      state.wifiScan.error = '设备 12 秒没有回应扫描。点「扫描」重试，或发 status 看链路。';
      renderWifiScan();
    }, 12000);
  }

  function armWifiConnectTimer() {
    clearWifiConnectTimer();
    wifiConnectTimer = setTimeout(function () {
      wifiConnectTimer = 0;
      if (!state.wifiConnecting) return;
      state.wifiConnecting = false;
      state.wifiLastReason = '设备 20 秒没有回连接结果。点「扫描」看热点是否可见，或发 status 查状态。';
      renderWifiStatus();
    }, 20000);
  }

  function probeWifiSupport() {
    if (wifiProbeTimer) clearTimeout(wifiProbeTimer);
    wifiProbeTimer = setTimeout(function () {
      wifiProbeTimer = 0;
      if (mode === 'live' && state.wifiSupported === null &&
          el.wifiDialog && el.wifiDialog.open) {
        state.wifiLastReason = '没收到设备的 wifi 回执：如果固件是旧的，请先烧录最新 firmware/。';
        renderWifiStatus();
      }
    }, 4000);
  }

  function addWifiAp(ap) {
    var items = state.wifiScan.items;
    var entry = { ssid: ap.ssid, rssi: ap.rssi, ch: ap.ch, match: !!ap.match };
    var found = -1;
    for (var i = 0; i < items.length; i++) {
      if (items[i].ssid === entry.ssid) { found = i; break; }
    }
    if (found >= 0) {
      if (entry.rssi > items[found].rssi) items[found] = entry;
    } else {
      items.push(entry);
    }
    items.sort(function (a, b) { return b.rssi - a.rssi; });
    renderWifiScan();
  }

  function renderWifiScan() {
    if (!el.wifiScanList) return;
    var scan = state.wifiScan;
    if (el.wifiScanNote) {
      if (scan.phase === 'scanning') {
        el.wifiScanNote.textContent = '扫描中…（约 2 秒）';
      } else if (scan.phase === 'done') {
        el.wifiScanNote.textContent = scan.items.length
          ? '点一个网络填入 SSID；共 ' + scan.items.length + ' 个可见，已按信号强弱排序。'
          : '没扫到网络：确认设备离路由器 3 m 内，且热点是 2.4 GHz。';
      } else if (scan.phase === 'error') {
        el.wifiScanNote.textContent = scan.error || '扫描失败。';
      } else {
        el.wifiScanNote.textContent = '点「扫描」列出附近网络，或直接输入 SSID。';
      }
    }

    var frag = document.createDocumentFragment();
    var current = el.wifiSsid && el.wifiSsid.value ? el.wifiSsid.value : '';
    for (var i = 0; i < scan.items.length; i++) {
      (function (item) {
        var li = document.createElement('li');
        var btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'wifi-ap';
        btn.dataset.ssid = item.ssid;
        if (item.ssid === current) btn.setAttribute('aria-pressed', 'true');
        var name = document.createElement('b');
        name.textContent = item.ssid;
        var meta = document.createElement('span');
        meta.textContent = item.rssi + ' dBm · ch' + item.ch + (item.match ? ' · 当前' : '');
        btn.appendChild(name);
        btn.appendChild(meta);
        btn.addEventListener('click', function () {
          if (el.wifiSsid) el.wifiSsid.value = item.ssid;
          renderWifiScan();
          if (el.wifiPass) el.wifiPass.focus();
        });
        li.appendChild(btn);
        frag.appendChild(li);
      })(scan.items[i]);
    }
    el.wifiScanList.replaceChildren(frag);
  }

  function renderWifiStatus() {
    if (el.wifiBtn) {
      el.wifiBtn.dataset.state = state.wifiIp ? 'on' : (mode === 'live' ? 'off' : 'idle');
      el.wifiBtn.title = state.wifiIp
        ? 'WiFi 已连接：' + state.wifiSsid + ' · ' + state.wifiIp
        : '配置设备 WiFi';
    }
    if (el.wifiInfo) {
      el.wifiInfo.textContent = state.wifiIp
        ? state.wifiIp + (state.wifiRssi !== null ? ' · ' + state.wifiRssi + ' dBm' : '')
        : (state.wifiSsid ? '未连接' + (state.wifiStatus !== null ? '（状态 ' + state.wifiStatus + '）' : '') : '—');
      el.wifiInfo.dataset.on = state.wifiIp ? 'on' : (state.wifiSsid || state.wifiStatus !== null ? 'off' : 'idle');
    }
    if (!el.wifiDialog || !el.wifiDialog.open) return;

    var live = mode === 'live';
    var canSend = live && state.wifiSupported !== false;
    var status, statusState;
    if (!serialSupported()) {
      status = '当前浏览器不支持 Web Serial，请用 Chrome / Edge 桌面版打开。';
      statusState = 'error';
    } else if (!live) {
      status = '还没连接设备。先点右上角「连接设备」，选 COM 口后再回来。';
      statusState = 'off';
    } else if (state.wifiSupported === false) {
      status = '当前固件不认识 wifi 命令，先烧录最新 firmware/ 再试。';
      statusState = 'error';
    } else if (state.wifiIp) {
      status = '设备已联网 · ' + (state.wifiSsid || 'SSID 未知') + ' · ' + state.wifiIp +
        (state.wifiRssi !== null ? ' · ' + state.wifiRssi + ' dBm' : '');
      statusState = 'on';
    } else if (state.wifiConnecting) {
      status = '正在连接 ' + (state.wifiSsid || 'WiFi') + '…';
      statusState = 'off';
    } else if (state.wifiSsid) {
      status = '设备未联网 · 已保存「' + state.wifiSsid + '」' +
        (state.wifiStatus !== null ? ' · 状态 ' + state.wifiStatus : '');
      statusState = 'off';
    } else {
      status = '设备还没配置 WiFi。输入 SSID 和密码后点「保存并连接」。';
      statusState = 'off';
    }
    if (el.wifiDialogStatus.textContent !== status) el.wifiDialogStatus.textContent = status;
    el.wifiDialogStatus.dataset.state = statusState;

    if (el.wifiHint) {
      var hint = '';
      var hintState = '';
      if (state.wifiLastReason) {
        hint = state.wifiLastReason;
        hintState = 'error';
      } else if (!live) {
        hint = '连接设备后才能发送配置命令。';
      } else if (state.wifiConnecting) {
        hint = '正在等待设备联网，最多 20 秒。';
        hintState = 'busy';
      } else if (state.wifiScan.phase === 'scanning') {
        hint = '正在扫描，约 2 秒。';
        hintState = 'busy';
      } else {
        hint = '密码只写入设备，页面不回显、不上传。';
      }
      if (el.wifiHint.textContent !== hint) el.wifiHint.textContent = hint;
      el.wifiHint.dataset.state = hintState;
    }

    if (el.wifiConnectBtn) {
      el.wifiConnectBtn.disabled = !canSend || state.wifiConnecting;
      el.wifiConnectBtn.textContent = state.wifiConnecting ? '正在连接…' : '保存并连接';
    }
    if (el.wifiScanBtn) el.wifiScanBtn.disabled = !canSend || state.wifiScan.phase === 'scanning';
    if (el.wifiClearBtn) el.wifiClearBtn.disabled = !canSend;
  }

  function renderWifi() {
    renderWifiStatus();
    renderWifiScan();
  }

  function openWifiDialog() {
    if (!el.wifiDialog) return;
    if (typeof el.wifiDialog.showModal === 'function') {
      if (!el.wifiDialog.open) el.wifiDialog.showModal();
    } else if (el.wifiDialog.setAttribute) {
      el.wifiDialog.setAttribute('open', '');
    }
    if (el.wifiSsid && !el.wifiSsid.value && state.wifiSsid) el.wifiSsid.value = state.wifiSsid;
    renderWifi();
    if (mode === 'live') {
      sendLine('wifi status\n').then(function (ok) { if (ok) probeWifiSupport(); });
    }
    setTimeout(function () { if (el.wifiSsid) el.wifiSsid.focus(); }, 40);
  }

  function closeWifiDialog() {
    if (!el.wifiDialog) return;
    if (typeof el.wifiDialog.close === 'function' && el.wifiDialog.open) el.wifiDialog.close();
    else if (el.wifiDialog.removeAttribute) el.wifiDialog.removeAttribute('open');
  }

  function toggleWifiPassword() {
    if (!el.wifiPass) return;
    var show = el.wifiPass.type === 'password';
    el.wifiPass.type = show ? 'text' : 'password';
    if (el.wifiPassToggle) {
      el.wifiPassToggle.textContent = show ? '隐藏' : '显示';
      el.wifiPassToggle.setAttribute('aria-label', show ? '隐藏密码' : '显示密码');
    }
  }

  function submitWifi(ev) {
    if (ev && ev.preventDefault) ev.preventDefault();
    if (mode !== 'live') { toast('先连接设备再配置 WiFi'); return; }
    if (state.wifiSupported === false) { toast('当前固件不支持 wifi 命令，请先烧录新固件'); return; }

    var ssid = el.wifiSsid && el.wifiSsid.value ? el.wifiSsid.value.trim() : '';
    var pass = el.wifiPass && el.wifiPass.value ? el.wifiPass.value : '';
    if (!ssid) {
      state.wifiLastReason = '请先填 WiFi 名称（SSID）。';
      renderWifiStatus();
      if (el.wifiSsid) el.wifiSsid.focus();
      return;
    }
    if (utf8Length(ssid) > 32) {
      state.wifiLastReason = 'SSID 最长 32 字节（中文约 10 个字）。';
      renderWifiStatus();
      return;
    }
    if (utf8Length(pass) > 63) {
      state.wifiLastReason = '密码最长 63 字节。';
      renderWifiStatus();
      return;
    }

    var cmd = 'wifi connect ' + encodeURIComponent(ssid) + ' ' +
      (pass ? commandSafePassword(pass) : '-') + '\n';
    state.wifiSsid = ssid;
    state.wifiConnecting = true;
    state.wifiLastReason = '';
    renderWifiStatus();
    sendLine(cmd).then(function (ok) {
      if (!ok) {
        state.wifiConnecting = false;
        state.wifiLastReason = '串口写入失败：重新点「连接设备」再试。';
        renderWifiStatus();
        return;
      }
      armWifiConnectTimer();
    });
  }

  function scanWifi() {
    if (mode !== 'live') { toast('先连接设备再扫描'); return; }
    if (state.wifiSupported === false) { toast('当前固件不支持 wifi 命令，请先烧录新固件'); return; }
    state.wifiScan.phase = 'scanning';
    state.wifiScan.expected = -1;
    state.wifiScan.items = [];
    state.wifiScan.error = '';
    renderWifiScan();
    renderWifiStatus();
    startWifiScanTimer();
    sendLine('wifi scan\n').then(function (ok) {
      if (ok) return;
      clearWifiScanTimer();
      state.wifiScan.phase = 'error';
      state.wifiScan.error = '串口写入失败：重新连接设备后再试。';
      renderWifiScan();
    });
  }

  function clearWifiSaved() {
    if (mode !== 'live') { toast('先连接设备再清除'); return; }
    if (state.wifiSupported === false) { toast('当前固件不支持 wifi 命令，请先烧录新固件'); return; }
    var ask = true;
    if (typeof window !== 'undefined' && typeof window.confirm === 'function') {
      ask = window.confirm('清除设备里保存的 WiFi 凭据？清除后设备会断开，除非 secrets.h 里还有一份。');
    }
    if (!ask) return;
    sendLine('wifi clear\n').then(function (ok) {
      if (ok) {
        toast('已发送清除命令');
      } else {
        state.wifiLastReason = '串口写入失败。';
        renderWifiStatus();
      }
    });
  }

  function resetWifiSession() {
    state.wifiIp = '';
    state.wifiRssi = null;
    state.wifiStatus = null;
    state.wifiConnecting = false;
    state.wifiSupported = null;
    state.wifiLastReason = '';
    state.wifiScan.phase = 'idle';
    state.wifiScan.items = [];
    state.wifiScan.error = '';
    clearWifiTimers();
    renderWifiStatus();
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
        resetWifiSession();
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
    resetWifiSession();
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

    // 顶栏 WiFi 指示 + 面板状态（1 Hz 刷新，避免每帧重排扫描列表）
    renderWifiStatus();
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

    if (el.wifiBtn) el.wifiBtn.addEventListener('click', openWifiDialog);
    if (el.wifiCloseBtn) el.wifiCloseBtn.addEventListener('click', closeWifiDialog);
    if (el.wifiDialog) {
      // 点遮罩关闭（native dialog 里遮罩上的点击 target 就是 dialog 本身）
      el.wifiDialog.addEventListener('click', function (ev) {
        if (ev.target === el.wifiDialog) closeWifiDialog();
      });
    }
    if (el.wifiForm) el.wifiForm.addEventListener('submit', submitWifi);
    if (el.wifiScanBtn) el.wifiScanBtn.addEventListener('click', scanWifi);
    if (el.wifiClearBtn) el.wifiClearBtn.addEventListener('click', clearWifiSaved);
    if (el.wifiPassToggle) el.wifiPassToggle.addEventListener('click', toggleWifiPassword);
    if (el.wifiSsid) {
      el.wifiSsid.addEventListener('input', function () {
        if (state.wifiScan.items.length) renderWifiScan();
      });
    }

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
          pushEvent({ kind: 'system', text: '串口断开（设备拔出）', uptime: 0 });
          resetWifiSession();
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
