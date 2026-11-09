#!/usr/bin/env python3
"""读开发板串口输出（只读，不发任何数据）。

用法：
    python read_serial.py [COM5] [秒数]

依赖 pyserial，IDF 的 python env 里自带：
    D:\\Espressif\\python_env\\idf5.4_py3.11_env\\Scripts\\python.exe read_serial.py COM5 15
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM5"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 15.0

ser = serial.Serial(port, 115200, timeout=1)
print(f"--- listening {port} for {seconds:.0f}s ---")
started = time.time()
try:
    while time.time() - started < seconds:
        raw = ser.readline()
        if not raw:
            continue
        text = raw.decode("utf-8", "replace").rstrip()
        if text:
            print(text, flush=True)
finally:
    ser.close()
    print("--- closed ---")
