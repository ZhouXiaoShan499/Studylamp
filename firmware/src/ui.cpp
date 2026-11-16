#include "ui.h"

#include <LittleFS.h>
#include <stdarg.h>

#if LAMP_USE_OLED
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

namespace {
Adafruit_SSD1306 display(128, 64, &Wire, -1);
bool oledReady = false;
}
#endif

namespace ui {
namespace {

CommandHandler handler = nullptr;
Publisher publisher = nullptr;
char line[384];
size_t lineLen = 0;
char statusText[64] = "boot";
char learnText[40] = "";
int8_t learnPct = -1;

constexpr size_t kLogMaxBytes = LOG_MAX_BYTES;
const char* kLogPath = "/log/events.log";
const char* kLogOldPath = "/log/events.old.log";

void rotateIfNeeded() {
    File file = LittleFS.open(kLogPath, FILE_READ);
    if (!file) return;
    const size_t size = file.size();
    file.close();
    if (size < kLogMaxBytes) return;
    LittleFS.remove(kLogOldPath);
    LittleFS.rename(kLogPath, kLogOldPath);
}

void drawOled() {
#if LAMP_USE_OLED
    if (!oledReady) return;
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("LAMP-1");
    display.setCursor(0, 16);
    display.println(statusText);
    if (learnText[0] != '\0') {
        display.setCursor(0, 32);
        display.println(learnText);
        if (learnPct >= 0) {
            display.drawRect(0, 46, 120, 10, SSD1306_WHITE);
            const int width = (120 * learnPct) / 100;
            display.fillRect(0, 46, width, 10, SSD1306_WHITE);
        }
    }
    display.display();
#else
    // OLED disabled: nothing to draw.
#endif
}

void dispatch(char* text) {
    char* argv[8];
    int argc = 0;
    char* cursor = text;
    while (*cursor != '\0' && argc < 8) {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == '\0') break;
        argv[argc++] = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') ++cursor;
        if (*cursor != '\0') *cursor++ = '\0';
    }
    if (argc == 0) return;
    argv[argc] = nullptr;
    if (handler != nullptr) {
        handler(argc, argv);
    } else {
        Serial.println("[ui] no command handler registered");
    }
}

}  // namespace

void begin() {
    Serial.begin(115200);
    delay(50);
    Serial.println();
    // FW_VERSION 是 constexpr const char*，不能和字符串字面量直接拼接
    Serial.printf("=== LAMP-1 MVP firmware %s (%s) ===\n", FW_VERSION, FW_BUILD);
    printHelp();

    if (!LittleFS.exists(LOG_DIR)) LittleFS.mkdir(LOG_DIR);
    rotateIfNeeded();

#if LAMP_USE_OLED
    Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
    oledReady = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    if (!oledReady) Serial.println("[ui] OLED not found, continuing headless");
    drawOled();
#endif
}

void setCommandHandler(CommandHandler value) { handler = value; }

void setPublisher(Publisher value) { publisher = value; }

void runCommand(const char* text) {
    if (text == nullptr) return;
    char copy[sizeof(line)];
    strncpy(copy, text, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    // 去掉尾部空白（网页偶尔会带 \r\n）
    size_t len = strlen(copy);
    while (len > 0 && (copy[len - 1] == '\r' || copy[len - 1] == '\n' || copy[len - 1] == ' ')) {
        copy[--len] = '\0';
    }
    if (len == 0) return;
    Serial.printf("> %s\n", copy);
    dispatch(copy);
}

void tick(uint32_t now_ms) {
    (void)now_ms;
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c < 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
            line[lineLen] = '\0';
            if (lineLen > 0) {
                Serial.printf("> %s\n", line);
                dispatch(line);
            }
            lineLen = 0;
            continue;
        }
        if (c == 8 || c == 127) {  // backspace
            if (lineLen > 0) --lineLen;
            continue;
        }
        if (lineLen + 1 < sizeof(line)) line[lineLen++] = static_cast<char>(c);
    }
}

void logf(const char* format, ...) {
    char buffer[192];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Serial.println(buffer);
    if (publisher != nullptr) publisher(buffer);

    File file = LittleFS.open(kLogPath, FILE_APPEND);
    if (!file) return;
    file.println(buffer);
    file.close();
}

void emitf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    Serial.println(buffer);
    if (publisher != nullptr) publisher(buffer);
}

void setStatus(const char* text) {
    if (text == nullptr) return;
    strncpy(statusText, text, sizeof(statusText) - 1);
    statusText[sizeof(statusText) - 1] = '\0';
    drawOled();
}

const char* status() { return statusText; }

void setLearnHint(const char* text, int8_t progress_pct) {
    if (text != nullptr) {
        strncpy(learnText, text, sizeof(learnText) - 1);
        learnText[sizeof(learnText) - 1] = '\0';
    } else {
        learnText[0] = '\0';
    }
    learnPct = progress_pct;
    drawOled();
}

void printHelp() {
    Serial.println(
        "commands:\n"
        "  status                     show lamp / ir / task summary\n"
        "  light <0-100>|on|off|toggle  brightness / power (same as the web buttons)\n"
        "  cct <2700-6500>            set colour temperature\n"
        "  mode study|office|rest|night\n"
        "  learn <slot>               learn one IR key into slot 0..15\n"
        "  learn/cancel               stop learning\n"
        "  send <slot>                transmit a stored key\n"
        "  slots                      list used IR slots\n"
        "  erase <slot>               delete a stored key\n"
        "  timer <seconds> [slot]     countdown, optionally also send IR\n"
        "  daily <HH:MM> [slot]       daily reminder (needs time sync)\n"
        "  tasks | del <id> | clear\n"
        "  time <epoch>               set the clock manually (testing)\n"
        "  motion nod|left|right|home|alert\n"
        "  mute | unmute              disable / enable motion\n"
        "  log                        dump the tail of the event log\n"
        "  mic on|off|test [seconds]  PDM mic level meter (voice task 1)\n"
        "  vad <rms> | vad 0          VAD sensitivity, 0 = restore default\n"
        "  key <API_KEY> [SECRET]     store cloud ASR key in NVS\n"
        "  wifi                       saved networks + current state\n"
        "  wifi scan                  list nearby 2.4 GHz APs\n"
        "  wifi connect <ssid> [pass] save and connect ('-' = open network)\n"
        "  wifi join <n>              connect to scan result n (saved password)\n"
        "  wifi use <n>               connect to saved network n\n"
        "  wifi forget <n|ssid>       drop one saved network\n"
        "  wifi auto                  re-run network selection now\n"
        "  wifi clear                 erase every saved WiFi credential\n"
        "  asr                        cloud ASR status (provider / key / token)\n"
        "  voice status|listen        link status / record one utterance\n"
        "  voice say <text>           run one text through the command matcher\n"
        "\n"
        "网页控制台：连上 WiFi 后打开 http://<设备IP>/ 或 http://lamp1.local/ ，\n"
        "没配网时设备自开热点 LAMP-1-xxxx（密码 lamp12345），手机直连 192.168.4.1。\n");
}

}  // namespace ui