#!/usr/bin/env python3
"""复位开发板并抓取完整启动日志。

用法：
    python reset_and_read.py [COM5] [秒数]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0

ser = serial.Serial(port, 115200, timeout=0.3)
# ESP32 自动复位序列：DTR 控制 GPIO0，RTS 控制 EN
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.15)
ser.setRTS(False)
time.sleep(0.05)
ser.setDTR(False)

print(f"--- reset {port}, capturing {seconds:.0f}s ---")
started = time.time()
try:
    while time.time() - started < seconds:
        raw = ser.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", "replace").rstrip("\r\n")
        if text:
            print(text, flush=True)
finally:
    ser.close()
    print("--- closed ---")
