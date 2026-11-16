#pragma once
// Serial command shell + event log + optional OLED status line.
//
// The serial console is not a debug afterthought: it is the only way to
// diagnose a unit in the field without an app, and it is what the automated
// acceptance tests drive. Keep every user-facing action reachable from it.

#include <Arduino.h>

#include "config.h"

namespace ui {

// argv[0] is the command, argv[argc] is guaranteed to be nullptr.
using CommandHandler = void (*)(int argc, char** argv);

void begin();
void tick(uint32_t now_ms);

void setCommandHandler(CommandHandler handler);

// WiFi 控制台旁路：每一行事件同时推给网页（由 net_console 注册）。
// 不注册时行为和纯串口固件完全一致。
using Publisher = void (*)(const char* line);
void setPublisher(Publisher publisher);

// Mirrors to Serial and appends one JSON line to /log/events.log.
void logf(const char* format, ...);

// 串口 + 网页都会出现的一行，但不写 /log/events.log。
// 用于对话类临时输出（[ASR] / [REPLY]），避免把事件日志塞满。
void emitf(const char* format, ...);

// 执行一行命令：串口输入和网页命令走同一条分词/分发路径。
// 命令字符串会被复制，调用方可传 const 指针；超长会截断。
void runCommand(const char* text);

// Short line shown on the OLED (and in `status`).
void setStatus(const char* text);
const char* status();

// Learn-progress feedback: "LEARN 2/6" + a bar.
void setLearnHint(const char* text, int8_t progress_pct);

void printHelp();

}  // namespace ui