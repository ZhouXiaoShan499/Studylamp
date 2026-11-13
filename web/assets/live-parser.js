/* LAMP—1 串口遥测解析层
 * 纯函数，无 DOM、无串口依赖，可在浏览器与 Node 里跑测试。
 *
 * 兼容两种固件：
 *   1. 当前固件（lamp_sr，只有 ESP_LOG 人读日志）——靠正则抽取
 *   2. 加了几行 # 行的固件——走快路径，字段齐全
 *
 * 契约：parseLine(line) 返回
 *   { type:'sample',  uptime, dbfs, rms?, peak?, src }      src: 'mic' | 'afe' | 'dev'
 *   { type:'state',   uptime, patch:{...} }                 brightness / mnState / vad / heapKb
 *   { type:'event',   uptime, kind, text, detail? }         kind 见 EVENT_KINDS
 *   { type:'hello',   uptime, fw, proto, caps }             #H 握手
 *   { type:'ignore',  reason }
 */
(function (root) {
  'use strict';

  /* ESP_LOG:  I (2078637) lamp_sr: diag: ...            */
  var LOG_RE = /^([IWE]) \((\d+)\)\s+[\w.-]+:\s?(.*)$/;
  /* #T,<ms>,<rms>,<peak>,<dbfs>,<vad>,<mn>,<heap>,<brightness> */
  var T_RE = /^#T,(\d+),([\d.]+),(-?[\d.]+),(-?[\d.]+),(-?\d+),(-?\d+)(?:,(\d+))?(?:,(\d+))?/;
  /* #S,<ms>,key=value,key=value */
  var S_RE = /^#S,(\d+),(.*)$/;
  /* #E,<ms>,<kind>,<text> */
  var E_RE = /^#E,(\d+),([a-z_]+),(.*)$/;
  /* #H,{"fw":"0.5.0","proto":1,"caps":["led","mic"]} */
  var H_RE = /^#H,(.+)$/;

  var RE_MIC = /mic peak=(-?\d+) rms=(-?\d+)/;
  var RE_AFE = /diag: afe_out=(\d+) bytes[^,]*,\s*ret=(\d+),\s*vol=(-?[\d.]+)/;
  var RE_AFE2 = /diag: inPeak=(-?\d+) outPeak=(-?\d+) ratio=([\d.]+)/;
  var RE_MNSTATE = /diag: mnState=(-?\d+)/;
  var RE_VAD = /vad=(\w+)\s*dbfs=(-?[\d.]+)/;
  var RE_MN_HIT = /MN id=(-?\d+) prob=([\d.]+) text=(\S+)/;
  var RE_LEVEL = /^>>> (\S+) level=(-?\d+) LED=(ON|OFF)/;
  var RE_WAKE = /\*\*\* wake word detected/;
  var RE_BOOT = /LAMP-SR ready|booting LAMP-SR/;
  var RE_TIMEOUT = /mn window expired/;
  var RE_BTN = /btn GPIO(\d+) idle=(-?\d+)/;
  var RE_ERR = /i2s read:/;

  var CMD_LABEL = { 1: '开灯', 2: '关灯', 3: '亮一点', 4: '暗一点', 5: '最亮' };

  function rmsToDbfs(rms) {
    return rms > 0 ? 20 * Math.log10(rms / 32768) : -120;
  }

  function ignore(reason) {
    return { type: 'ignore', reason: reason };
  }

  function parseStructured(line) {
    var m = T_RE.exec(line);
    if (m) {
      var uptime = parseInt(m[1], 10);
      var patch = {};
      if (m[7] !== undefined) patch.heapKb = parseInt(m[7], 10);
      if (m[8] !== undefined) patch.brightness = parseInt(m[8], 10);
      patch.mnState = parseInt(m[6], 10);
      patch.vad = parseInt(m[5], 10);
      var out = {
        type: 'sample',
        uptime: uptime,
        src: 'dev',
        rms: parseFloat(m[2]),
        peak: parseFloat(m[3]),
        dbfs: parseFloat(m[4]),
        patch: patch
      };
      return out;
    }
    m = S_RE.exec(line);
    if (m) {
      var st = parseInt(m[1], 10);
      var fields = m[2].split(',');
      var p = {};
      for (var i = 0; i < fields.length; i++) {
        var kv = fields[i].split('=');
        if (kv.length !== 2) continue;
        var k = kv[0].trim();
        var v = kv[1].trim();
        if (k === 'brightness' || k === 'mn' || k === 'vad' || k === 'heap') {
          var n = parseInt(v, 10);
          if (!isNaN(n)) {
            p[k === 'mn' ? 'mnState' : k === 'heap' ? 'heapKb' : k] = n;
          }
        }
      }
      return { type: 'state', uptime: st, patch: p };
    }
    m = E_RE.exec(line);
    if (m) {
      return { type: 'event', uptime: parseInt(m[1], 10), kind: m[2], text: m[3] };
    }
    m = H_RE.exec(line);
    if (m) {
      try {
        var h = JSON.parse(m[1]);
        return {
          type: 'hello',
          uptime: 0,
          fw: h.fw || '未知',
          proto: h.proto || 1,
          caps: Array.isArray(h.caps) ? h.caps : []
        };
      } catch (e) {
        return ignore('bad #H json');
      }
    }
    return null;
  }

  function parseLog(msg, uptime) {
    var m;

    m = RE_MIC.exec(msg);
    if (m) {
      var rms = parseInt(m[2], 10);
      return {
        type: 'sample', uptime: uptime, src: 'mic',
        peak: parseInt(m[1], 10), rms: rms, dbfs: rmsToDbfs(rms)
      };
    }

    m = RE_AFE.exec(msg);
    if (m) {
      return { type: 'sample', uptime: uptime, src: 'afe', dbfs: parseFloat(m[3]) };
    }

    m = RE_VAD.exec(msg);
    if (m) {
      return {
        type: 'state', uptime: uptime,
        patch: { vad: m[1] === 'speech' ? 1 : 0, vadDbfs: parseFloat(m[2]) }
      };
    }

    m = RE_MN_HIT.exec(msg);
    if (m) {
      var id = parseInt(m[1], 10);
      var prob = parseFloat(m[2]);
      return {
        type: 'event', uptime: uptime, kind: 'command',
        text: '命中「' + (CMD_LABEL[id] || ('id ' + id)) + '」 p=' + prob.toFixed(3) + ' · ' + m[3],
        detail: { cmdId: id, prob: prob, pinyin: m[3] }
      };
    }

    m = RE_LEVEL.exec(msg);
    if (m) {
      return {
        type: 'event', uptime: uptime, kind: 'led',
        text: m[1] + ' · level ' + parseInt(m[2], 10) + ' · LED ' + m[3],
        patch: { brightness: parseInt(m[2], 10) }
      };
    }

    if (RE_WAKE.test(msg)) {
      return { type: 'event', uptime: uptime, kind: 'wake', text: '唤醒词命中' };
    }
    if (RE_BOOT.test(msg)) {
      return { type: 'event', uptime: uptime, kind: 'boot', text: msg.slice(0, 80) };
    }
    if (RE_TIMEOUT.test(msg)) {
      return { type: 'event', uptime: uptime, kind: 'timeout', text: '识别窗口过期，已重开' };
    }
    if (RE_ERR.test(msg)) {
      return { type: 'event', uptime: uptime, kind: 'error', text: msg.slice(0, 80) };
    }

    m = RE_BTN.exec(msg);
    if (m) {
      return {
        type: 'event', uptime: uptime, kind: 'button',
        text: '按键 GPIO' + m[1] + ' 空闲电平 ' + m[2]
      };
    }

    m = RE_MNSTATE.exec(msg);
    if (m) {
      return { type: 'state', uptime: uptime, patch: { mnState: parseInt(m[1], 10) } };
    }

    if (RE_AFE2.test(msg)) return ignore('afe-ratio');
    if (/^DUMP /.test(msg)) return ignore('dump');
    if (/^add \[\d+\]/.test(msg)) return ignore('cmd-register');
    if (/^(afe |mic ready|model partition|MultiNet model|btn GPIO)/.test(msg) && !RE_BTN.test(msg)) {
      return ignore('boot-detail');
    }
    return ignore('unmatched');
  }

  /* =====================================================================
   * Arduino 固件（firmware/，provider=4 腾讯云直连）的串口行
   *
   * 这版固件不用 ESP_LOG，而是自己 printf：
   *   [voice] trigger rms=490 floor=81 thr=450
   *   [voice] recording (rms=490, preroll=2048)
   *   [asr] reply 133 B in 1054 ms
   *   [ASR] "开灯"                       ← 识别文本（要大字显示的）
   *   [LLM] "开灯" -> on                 ← 意图标签
   *   [cmd] intent="on" -> on=1 pct=60   ← 实际动作
   *   [mic] rms=    58 floor=  60 thr= 150 |#...| quiet
   *   [wifi] connected ssid="Xiaomi_037C" ip=192.168.31.1 rssi=-28
   *   [mem] psram=8386247 heap=224344
   *   {"ev":"light_set","pct":60,"src":"voice"}
   * ===================================================================== */
  var AR_MIC = /^\[mic\]\s+rms=\s*(\d+)\s+floor=\s*(\d+)\s+thr=\s*(\d+)/;
  var AR_TRIGGER = /^\[voice\]\s+trigger rms=([\d.]+)\s+floor=([\d.]+)\s+thr=([\d.]+)/;
  var AR_RECORDING = /^\[voice\]\s+recording/;
  var AR_ASR_TEXT = /^\[ASR\]\s+"?(.*?)"?\s*$/;
  var AR_LLM = /^\[LLM\]\s+"(.*)"\s*->\s*(\S+)\s*$/;
  var AR_CMD_INTENT = /^\[cmd\]\s+intent="([^"]*)"\s*->\s*on=(\d+)\s+pct=(\d+)/;
  var AR_CMD_TEXT = /^\[cmd\]\s+"(.*)"\s*->\s*on=(\d+)\s+pct=(\d+)/;
  var AR_LOCAL_CMD = /^\[CMD\]\s+"(.*)"\s*->\s*(\S+)\s+\(local\)/;
  var AR_REPLY = /^\[REPLY\]\s*(.*)$/;
  var AR_ASR_REPLY = /^\[asr\]\s+reply\s+(\d+)\s+B\s+in\s+(\d+)\s+ms/;
  var AR_WIFI = /^\[wifi\]\s+connected ssid="([^"]*)"\s+ip=(\S+)\s+rssi=(-?\d+)/;
  /* 重连失败时固件每 10 s 打一行 status，扫到的 AP 也是 [wifi] 前缀。
     这两类以前落到 ignore，恰好是「为什么连不上」的全部证据，现在收成事件。 */
  var AR_WIFI_STATUS = /^\[wifi\]\s+status=(-?\d+)\s+(.+)$/;
  var AR_WIFI_SCAN = /^\[wifi\]\s+scan:\s+(-?\d+)\s+AP/;
  var AR_WIFI_AP = /^\[wifi\]\s+(-?\d+)\s+dBm\s+ch(\d+)\s+(.+?)\s*(<== match)?$/;
  /* 网页「连接 WiFi」走的命令回执：凭据只存 NVS，密码永远不会回传。 */
  var AR_WIFI_SAVED = /^\[wifi\]\s+saved ssid="([^"]*)"\s+password=(\w+)/;
  var AR_WIFI_CONNECTING = /^\[wifi\]\s+connecting ssid="([^"]*)"/;
  var AR_WIFI_CLEARED = /^\[wifi\]\s+cleared\b/;
  var AR_WIFI_CRED = /^\[wifi\]\s+credentials ssid="([^"]*)"\s+source=(\S+)\s+password=(\w+)/;
  var AR_WIFI_CRED_NONE = /^\[wifi\]\s+(?:credentials none|no credentials)/;
  var AR_WIFI_FALLBACK = /^\[wifi\]\s+falling back to secrets\.h ssid="([^"]*)"/;
  var AR_WIFI_SCAN_DONE = /^\[wifi\]\s+scan done/;
  var AR_WIFI_SCAN_FAILED = /^\[wifi\]\s+scan failed code=(-?\d+)/;
  var AR_WIFI_BAD = /^\[wifi\]\s+bad ([a-z]+):\s*(.*)$/;
  var AR_UNKNOWN_CMD = /^unknown command:\s*(\S+)/;
  var AR_MEM = /^\[mem\]\s+psram=(\d+)\s+heap=(\d+)/;
  var AR_EMPTY = /^\[asr\]\s+empty result/;
  var AR_NOT_READY = /cloud ASR not ready|wifi down, dropped|no key, dropped/;
  var AR_JSON = /^\{.*"ev"\s*:/;

  function parseArduino(s) {
    var m;

    m = AR_TRIGGER.exec(s);
    if (m) {
      return {
        type: 'event', kind: 'trigger',
        text: '听到声音 · rms ' + Math.round(parseFloat(m[1])) + ' / 阈值 ' + Math.round(parseFloat(m[3])),
        patch: { threshold: parseFloat(m[3]), level: parseFloat(m[1]) }
      };
    }

    if (AR_RECORDING.test(s)) {
      return { type: 'event', kind: 'listen', text: '正在录音（2 秒）…' };
    }

    m = AR_ASR_TEXT.exec(s);
    if (m) {
      return { type: 'voice', text: m[1].trim() };
    }

    m = AR_LLM.exec(s);
    if (m) {
      return { type: 'intent', heard: m[1].trim(), intent: m[2] };
    }

    m = AR_CMD_INTENT.exec(s);
    if (m) {
      return { type: 'action', intent: m[1], on: m[2] === '1', pct: parseInt(m[3], 10) };
    }

    m = AR_LOCAL_CMD.exec(s);
    if (m) {
      return { type: 'intent', heard: m[1].trim(), intent: m[2], local: true };
    }

    m = AR_REPLY.exec(s);
    if (m) {
      return { type: 'reply', text: m[1].trim() };
    }

    m = AR_CMD_TEXT.exec(s);
    if (m) {
      return { type: 'action', intent: m[1], on: m[2] === '1', pct: parseInt(m[3], 10) };
    }

    m = AR_ASR_REPLY.exec(s);
    if (m) {
      return { type: 'stat', asrBytes: parseInt(m[1], 10), asrMs: parseInt(m[2], 10) };
    }

    m = AR_MIC.exec(s);
    if (m) {
      var rms = parseInt(m[1], 10);
      return {
        type: 'sample', src: 'mic',
        rms: rms, dbfs: rmsToDbfs(rms),
        floor: parseInt(m[2], 10), threshold: parseInt(m[3], 10)
      };
    }

    m = AR_WIFI.exec(s);
    if (m) {
      return { type: 'link', ssid: m[1], ip: m[2], rssi: parseInt(m[3], 10) };
    }

    m = AR_WIFI_SAVED.exec(s);
    if (m) {
      return {
        type: 'wifiSaved', ssid: m[1],
        hasPassword: m[2] !== 'empty'
      };
    }

    m = AR_WIFI_CONNECTING.exec(s);
    if (m) {
      return { type: 'wifiConnecting', ssid: m[1] };
    }

    if (AR_WIFI_CLEARED.test(s)) {
      return { type: 'wifiCleared' };
    }

    m = AR_WIFI_CRED.exec(s);
    if (m) {
      return {
        type: 'wifiCred', ssid: m[1], source: m[2],
        hasPassword: m[3] !== 'empty'
      };
    }

    m = AR_WIFI_FALLBACK.exec(s);
    if (m) {
      return { type: 'wifiCred', ssid: m[1], source: 'secrets.h', hasPassword: true };
    }

    if (AR_WIFI_CRED_NONE.test(s)) {
      return { type: 'wifiCredNone' };
    }

    m = AR_WIFI_BAD.exec(s);
    if (m) {
      return { type: 'wifiError', field: m[1], detail: m[2].trim() };
    }

    m = AR_WIFI_STATUS.exec(s);
    if (m) {
      var wst = parseInt(m[1], 10);
      return {
        type: 'event', kind: wst === 3 ? 'link' : 'warn',
        wifiStatus: wst,
        text: 'WiFi 状态 ' + wst + ' · ' + m[2].trim()
      };
    }

    m = AR_WIFI_SCAN.exec(s);
    if (m) {
      // 旧固件扫描失败会打 scan: -2 AP(s)，别当成「看到 -2 个网络」。
      var apCount = parseInt(m[1], 10);
      return apCount < 0
        ? { type: 'wifiScanFailed', code: apCount }
        : { type: 'wifiScan', count: apCount };
    }

    m = AR_WIFI_SCAN_FAILED.exec(s);
    if (m) {
      return { type: 'wifiScanFailed', code: parseInt(m[1], 10) };
    }

    if (AR_WIFI_SCAN_DONE.test(s)) {
      return { type: 'wifiScanDone' };
    }

    m = AR_WIFI_AP.exec(s);
    if (m) {
      var apName = m[3].trim();
      return {
        type: 'wifiAp',
        ssid: apName === '<hidden>' ? '' : apName,
        hidden: apName === '<hidden>',
        rssi: parseInt(m[1], 10),
        ch: parseInt(m[2], 10),
        match: /<==/.test(s)
      };
    }

    m = AR_MEM.exec(s);
    if (m) {
      return { type: 'health', psram: parseInt(m[1], 10), heap: parseInt(m[2], 10) };
    }

    if (AR_EMPTY.test(s)) {
      return { type: 'event', kind: 'empty', text: '没听到有效人声（空结果）' };
    }

    if (AR_NOT_READY.test(s)) {
      return { type: 'event', kind: 'warn', text: s.slice(0, 90) };
    }

    if (/^\[voice\]\s*(无动作标签|没匹配到命令|未识别的动作标签)/.test(s)) {
      return { type: 'event', kind: 'warn', text: s.slice(0, 90) };
    }

    if (AR_JSON.test(s)) {
      try {
        var ev = JSON.parse(s);
        if (ev.ev === 'light_set') {
          return {
            type: 'action', intent: 'light', on: ev.pct > 0, pct: ev.pct,
            source: ev.src || ''
          };
        }
        if (ev.ev === 'light_preset') {
          return { type: 'event', kind: 'action', text: '预设 ' + ev.name };
        }
        if (ev.ev === 'wifi_ok') {
          return { type: 'event', kind: 'link', text: 'WiFi 已连接 ' + (ev.ip || '') };
        }
        if (ev.ev === 'boot') {
          return {
            type: 'event', kind: 'boot',
            text: '固件启动 ' + (ev.fw || '') + ' · heap ' + (ev.heap || ''),
            fw: ev.fw || ''
          };
        }
      } catch (e) {
        return ignore('bad-json');
      }
      return { type: 'event', kind: 'system', text: s.slice(0, 90) };
    }

    if (/^\[status\]/.test(s) || /^\[voice\] cloud=/.test(s) || /^\[asr\] provider=/.test(s) ||
        /^\[llm\]/.test(s)) {
      return { type: 'status', text: s.replace(/^\[[a-z]+\]\s*/, '').slice(0, 120) };
    }

    // 旧固件收到 wifi 命令会回这一行：网页据此提示「需要重新烧录」。
    m = AR_UNKNOWN_CMD.exec(s);
    if (m) {
      return { type: 'unknownCommand', command: m[1] };
    }

    return null;
  }

  function parseLine(line) {
    if (typeof line !== 'string') return ignore('non-string');
    var s = line.replace(/\r$/, '').replace(/\u001b\[[0-9;]*m/g, '').trim();
    if (!s) return ignore('empty');

    // Arduino 固件（firmware/）的人读日志优先
    var ar = parseArduino(s);
    if (ar) return ar;

    // 结构化行优先：固件加了几行之后走这里，字段完整、频率高
    if (s.charAt(0) === '#') {
      var structured = parseStructured(s);
      if (structured) return structured;
    }

    // 启动期的裸日志（没有 "I (ms) tag:" 前缀）
    if (/^(ESP-ROM|rst:0x|I \(|W \(|E \()/.test(s) === false && /^(boot|LAMP-SR)/i.test(s)) {
      return parseLog(s, 0);
    }

    var m = LOG_RE.exec(s);
    if (!m) {
      // 当前固件的部分日志不带 tag 前缀（例如 ROM 引导、rst 原因）
      return ignore('no-log-prefix');
    }
    var level = m[1];
    var uptime = parseInt(m[2], 10);
    var msg = m[3];

    var parsed = parseLog(msg, uptime);
    if (parsed && parsed.type === 'ignore' && level === 'E') {
      return { type: 'event', uptime: uptime, kind: 'error', text: msg.slice(0, 80) };
    }
    return parsed || ignore('unmatched');
  }

  var API = {
    parseLine: parseLine,
    rmsToDbfs: rmsToDbfs,
    CMD_LABEL: CMD_LABEL,
    EVENT_KINDS: ['boot', 'wake', 'command', 'led', 'button', 'timeout', 'error']
  };

  if (typeof module === 'object' && module.exports) {
    module.exports = API;      // Node 里 require 做测试
  } else {
    root.LampParser = API;     // 浏览器里挂到 window
  }
})(typeof globalThis !== 'undefined' ? globalThis : this);
