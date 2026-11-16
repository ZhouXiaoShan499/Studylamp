#include "net_console.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Esp.h>
#include <Esp.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "cloud_asr.h"
#include "config.h"
#include "console_html.h"
#include "lighting.h"
#include "pdm_mic.h"
#include "ui.h"
#include "voice_client.h"
#include "wifi_link.h"

namespace netconsole {
namespace {

// 页面在 80，WebSocket 单独占 81：WebServer 的升级头处理起来麻烦，
// 分开两个端口最省事，页面里写死 ws://<host>:81/ 即可。
WebServer server(80);
WebSocketsServer socket(81);
DNSServer dns;

bool apRunning = false;
bool mdnsStarted = false;
bool loggedOnline = false;
uint32_t lastMdnsTry = 0;

uint32_t bootAt = 0;
uint32_t lastApTry = 0;
uint32_t lastTickPush = 0;

// 连不上路由多久之后开热点。wifiTick() 每 10 秒重试一次，25 秒约等于两次失败。
constexpr uint32_t kApFallbackMs = 25000;
constexpr uint32_t kTickPushMs = 20000;
constexpr const char* kHostname = "lamp1";
// 台架热点密码。要改直接改这里；产品化时应改成按键触发 + 每台独立密码。
constexpr const char* kApPassword = "lamp12345";

// 热点名带 MAC 后 3 字节，桌上同时有两台也不撞名。
String apSsid() {
    String mac = WiFi.macAddress();  // AA:BB:CC:DD:EE:FF
    mac.replace(":", "");
    String suffix = mac.length() >= 6 ? mac.substring(mac.length() - 6) : mac;
    suffix.toUpperCase();
    return String("LAMP-1-") + suffix;
}

String currentIp() {
    if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
    if (apRunning) return WiFi.softAPIP().toString();
    return String();
}

// ---------------------------------------------------------------- AP / mDNS ---
void startAccessPoint(uint32_t now) {
    if (apRunning) return;
    // softAP 失败时不要每轮 loop 都重试：会刷串口、也影响射频状态。
    if (lastApTry != 0 && (now - lastApTry) < 10000) return;
    lastApTry = now;
    // AP_STA 而不是 AP：热点开着的同时仍然继续重试路由器，配完网不用重启。
    WiFi.mode(WIFI_AP_STA);
    const String ssid = apSsid();
    if (!WiFi.softAP(ssid.c_str(), kApPassword)) {
        Serial.println("[net] softAP start failed");
        return;
    }
    delay(100);  // 等 DHCP 服务起来，否则第一台手机可能拿不到地址
    apRunning = true;
    dns.start(53, "*", WiFi.softAPIP());
    Serial.printf("[net] AP up ssid=\"%s\" pass=\"%s\" ip=%s\n", ssid.c_str(), kApPassword,
                  WiFi.softAPIP().toString().c_str());
    ui::logf("{\"ev\":\"ap_up\",\"ssid\":\"%s\",\"ip\":\"%s\"}", ssid.c_str(),
             WiFi.softAPIP().toString().c_str());
}

void ensureMdns(uint32_t now) {
    if (mdnsStarted) return;
    if (WiFi.status() != WL_CONNECTED && !apRunning) return;
    // 失败时 5 秒后再试：MDNS.begin() 会起 udp 监听，每轮 loop 都调一次不值当。
    if (lastMdnsTry != 0 && (now - lastMdnsTry) < 5000) return;
    lastMdnsTry = now;
    if (MDNS.begin(kHostname)) {
        MDNS.addService("http", "tcp", 80);
        mdnsStarted = true;
        Serial.printf("[net] mdns http://%s.local/\n", kHostname);
    }
}

// ------------------------------------------------------------------- 路由 ---
void sendHtml(bool gzip) {
    if (gzip) {
        server.sendHeader("Content-Encoding", "gzip");
        server.sendHeader("Vary", "Accept-Encoding");
        server.send_P(200, "text/html; charset=utf-8",
                      reinterpret_cast<PGM_P>(kConsoleHtmlGz), kConsoleHtmlGzLen);
    } else {
        server.send_P(200, "text/html; charset=utf-8", reinterpret_cast<PGM_P>(kConsoleHtml),
                      kConsoleHtmlLen);
    }
}

void handleRoot() {
    const String accept = server.header("Accept-Encoding");
    sendHtml(accept.indexOf("gzip") >= 0);
}

void handleState() {
    JsonDocument doc;
    doc["fw"] = FW_VERSION;
    doc["uptime"] = static_cast<uint32_t>(millis() / 1000UL);
    doc["heap"] = ESP.getFreeHeap();
    doc["clients"] = socket.connectedClients();

    JsonObject net = doc["net"].to<JsonObject>();
    const bool staUp = wifilink::online();
    net["mode"] = staUp ? (apRunning ? "ap+sta" : "sta") : (apRunning ? "ap" : "down");
    net["ssid"] = staUp ? wifilink::activeSsid() : String("");
    net["ip"] = wifilink::ip();
    net["rssi"] = wifilink::rssi();
    net["ap"] = apRunning;
    net["host"] = String(kHostname) + ".local";
    // 页面靠这三个字段区分「正在扫描/连接」和「真的没网」，
    // 否则换网那 10 秒里界面会一直显示「未联网」。
    net["phase"] = wifilink::phase();
    net["saved"] = static_cast<uint32_t>(wifilink::savedCount());
    net["last"] = wifilink::lastIndex() >= 0
                      ? wifilink::savedSsid(static_cast<size_t>(wifilink::lastIndex()))
                      : String("");

    JsonObject light = doc["light"].to<JsonObject>();
    light["on"] = lighting::isOn();
    light["pct"] = lighting::brightness();
    light["cct"] = lighting::cct();
    light["preset"] = lighting::presetName(lighting::preset());

    JsonObject voice = doc["voice"].to<JsonObject>();
    voice["provider"] = cloudasr::providerName();
    voice["ready"] = voice::connected();
    voice["listening"] = voice::listening();
    voice["mic"] = pdmmic::ready();

    String out;
    serializeJson(doc, out);
    server.send(200, "application/json", out);
}

// 网页命令的兜底通道：WebSocket 断线时用 /api/cmd?c=light%2060。
// 和串口命令完全同一条分发路径，所以 light / mode / voice listen / wifi connect
// 这些命令在两边行为一致，不需要为网页再写一套。
void handleCmd() {
    String cmd;
    if (server.hasArg("c")) {
        cmd = server.arg("c");
    } else if (server.hasArg("plain")) {
        cmd = server.arg("plain");
    }
    cmd.trim();
    if (cmd.isEmpty()) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"empty command\"}");
        return;
    }
    ui::runCommand(cmd.c_str());
    server.send(200, "application/json", "{\"ok\":true}");
}

// --------------------------------------------------------------- WiFi 接口 ---
// 设备页的「附近的网络」。扫描会占用射频 1~5 秒，只在页面展开/点刷新时调，
// 不进 /api/state —— 否则每 15 秒一次状态轮询会把语音链路打断。
void handleWifiScan() {
    const int n = wifilink::scan();
    JsonDocument doc;
    doc["ok"] = n >= 0;
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n; ++i) {
        JsonObject ap = arr.add<JsonObject>();
        ap["ssid"] = wifilink::scanSsid(i);
        ap["rssi"] = wifilink::scanRssi(i);
        ap["ch"] = wifilink::scanChannel(i);
        ap["secure"] = wifilink::scanSecure(i);
        ap["saved"] = wifilink::scanKnownIndex(i) >= 0;
    }
    String out;
    serializeJson(doc, out);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", out);
}

void handleWifiNetworks() {
    JsonDocument doc;
    doc["ok"] = true;
    doc["online"] = wifilink::online();
    doc["ssid"] = wifilink::activeSsid();
    doc["ip"] = wifilink::ip();
    doc["phase"] = wifilink::phase();
    const int last = wifilink::lastIndex();
    JsonArray arr = doc["saved"].to<JsonArray>();
    for (size_t i = 0; i < wifilink::savedCount(); ++i) {
        JsonObject item = arr.add<JsonObject>();
        item["ssid"] = wifilink::savedSsid(i);
        item["last"] = static_cast<int>(i) == last;
        item["active"] = wifilink::online() && wifilink::activeSsid() == wifilink::savedSsid(i);
    }
    String out;
    serializeJson(doc, out);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", out);
}

// body: {"ssid":"...","pass":"..."}，或 {"ssid":"...","saved":true} 用存好的密码。
// 只保存并开始连，不在这里等结果：12 秒的连接过程会把 HTTP 拖到超时。
void handleWifiConnect() {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
        return;
    }
    const String ssid = doc["ssid"] | "";
    if (ssid.isEmpty()) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ssid\"}");
        return;
    }

    bool ok = false;
    if (!doc["pass"].isNull()) {
        ok = wifilink::connect(ssid, doc["pass"] | "");
    } else {
        for (size_t i = 0; i < wifilink::savedCount(); ++i) {
            if (wifilink::savedSsid(i) == ssid) {
                ok = wifilink::connectSaved(i);
                break;
            }
        }
    }
    server.send(ok ? 200 : 400, "application/json",
                ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"connect rejected\"}");
}

// body: {"ssid":"..."}
void handleWifiForget() {
    JsonDocument doc;
    if (deserializeJson(doc, server.arg("plain")) != DeserializationError::Ok) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
        return;
    }
    const String ssid = doc["ssid"] | "";
    const bool ok = wifilink::forget(ssid);
    server.send(ok ? 200 : 404, "application/json",
                ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"not saved\"}");
}

void handleNotFound() {
    // 热点模式下把任意域名都指回控制台，手机才会弹「登录网络」。
    if (apRunning && currentIp().length() > 0) {
        server.sendHeader("Location", String("http://") + currentIp() + "/");
        server.send(302, "text/plain", "");
        return;
    }
    server.send(404, "application/json", "{\"ok\":false,\"error\":\"not found\"}");
}

// ----------------------------------------------------------------- websocket ---
void onSocketEvent(uint8_t client, WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED: {
            const IPAddress ip = socket.remoteIP(client);
            Serial.printf("[net] ws client #%u from %s\n", static_cast<unsigned>(client),
                          ip.toString().c_str());
            String hello = String("{\"type\":\"hello\",\"fw\":\"") + FW_VERSION + "\",\"host\":\"" +
                           kHostname + ".local\"}";
            socket.sendTXT(client, hello);
            break;
        }

        case WStype_DISCONNECTED:
            Serial.printf("[net] ws client #%u left\n", static_cast<unsigned>(client));
            break;

        case WStype_TEXT: {
            String text(reinterpret_cast<const char*>(payload), length);
            text.trim();
            if (text.isEmpty()) break;

            // 两种写法都收：{"cmd":"light 60"} 和裸的 light 60。
            // 后者方便 curl / 调试工具直接发，不用包 JSON。
            if (text[0] == '{') {
                JsonDocument doc;
                if (deserializeJson(doc, text) != DeserializationError::Ok) {
                    socket.sendTXT(client, "{\"type\":\"error\",\"message\":\"bad json\"}");
                    break;
                }
                const char* cmd = doc["cmd"] | "";
                if (cmd[0] == '\0') {
                    socket.sendTXT(client, "{\"type\":\"error\",\"message\":\"missing cmd\"}");
                    break;
                }
                ui::runCommand(cmd);
            } else {
                ui::runCommand(text.c_str());
            }
            break;
        }

        default:
            break;
    }
}

}  // namespace

void begin(bool hasCredentials) {
    // 参数保留给调用方表达「启动时有没有凭据」，实际判断一律走 wifilink：
    // 用户在热点页面里配完网之后，凭据表会变，静态快照会过期。
    (void)hasCredentials;
    bootAt = millis();

    // mDNS 名字要在 DHCP 之前设好，否则路由器里显示的是默认 esp32-xxxx。
    WiFi.setHostname(kHostname);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/state", HTTP_GET, handleState);
    server.on("/api/cmd", HTTP_GET, handleCmd);
    server.on("/api/cmd", HTTP_POST, handleCmd);
    server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
    server.on("/api/wifi/networks", HTTP_GET, handleWifiNetworks);
    server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
    server.on("/api/wifi/forget", HTTP_POST, handleWifiForget);
    server.on("/favicon.ico", HTTP_GET, []() { server.send(204, "text/plain", ""); });
    server.onNotFound(handleNotFound);
    server.begin();

    socket.begin();
    socket.onEvent(onSocketEvent);

    Serial.printf("[net] console on port 80 (ws 81), sta=%s\n",
                  wifilink::hasCredentials() ? "configured" : "absent");
    if (!wifilink::hasCredentials()) {
        // 从没配过网：直接开热点，手机连上就能配，不必先接 USB。
        startAccessPoint(millis());
    }
}

void tick(uint32_t now) {
    server.handleClient();
    socket.loop();
    if (apRunning) dns.processNextRequest();

    // 有凭据却一直连不上（密码错 / 路由器不在 2.4 GHz / 换了环境）：
    // 到 25 秒还没有 IP 就把热点开起来，否则现场会陷入「设备不响应、又没串口」。
    if (!apRunning && !wifilink::hasCredentials()) startAccessPoint(now);
    // 有凭据却一直连不上（密码错 / 路由器不在 2.4 GHz / 换了环境）：
    // 到 25 秒还没有 IP 就把热点开起来，否则现场会陷入「设备不响应、又没串口」。
    // 正在连接/扫描时先不动：WiFi.mode() 会把射频重配一遍，正好打断那次尝试。
    // 所以再加一个 60 秒硬上限，避免极端情况下热点永远不开。
    const uint32_t sinceBoot = now - bootAt;
    if (!apRunning && !wifilink::online()) {
        const bool canBreakIn = !wifilink::busy() || sinceBoot >= kApFallbackMs + 35000;
        if (sinceBoot >= kApFallbackMs && canBreakIn) {
            Serial.println("[net] STA not up, falling back to SoftAP");
            startAccessPoint(now);
        }
    }

    ensureMdns(now);

    if (wifilink::online() && !loggedOnline) {
        loggedOnline = true;
        Serial.printf("[net] console ready: http://%s/  (http://%s.local/)\n",
                      wifilink::ip().c_str(), kHostname);
        ui::logf("{\"ev\":\"console_up\",\"ip\":\"%s\",\"ap\":%d}", wifilink::ip().c_str(),
                 apRunning ? 1 : 0);
    } else if (!wifilink::online()) {
        loggedOnline = false;
    }

    // 心跳：手机锁屏 / 路由器 NAT 超时后，浏览器能靠它发现连接已死并重连。
    if ((now - lastTickPush) >= kTickPushMs) {
        lastTickPush = now;
        if (socket.connectedClients() > 0) {
            char line[64];
            snprintf(line, sizeof(line), "{\"type\":\"tick\",\"up\":%lu}",
                     static_cast<unsigned long>(millis() / 1000UL));
            socket.broadcastTXT(line);
        }
    }
}

void publish(const char* line) {
    if (line == nullptr || line[0] == '\0') return;
    if (socket.connectedClients() == 0) return;
    socket.broadcastTXT(line);
}

bool apActive() { return apRunning; }

String url() {
    const String ip = currentIp();
    if (ip.isEmpty()) return String();
    return String("http://") + ip + "/";
}

}  // namespace netconsole
