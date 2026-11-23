# lamp_av：唯一的 ESP-IDF 灯固件工程（docs/42 的固定值）
#
# 不要用系统里的 idf.py —— 用本目录的 idf.bat，它把 IDF 5.4.3 与 python venv 钉死：
#   .\idf.bat set-target esp32s3
#   .\idf.bat build
#   .\idf.bat -p COM5 flash
#
# 目标芯片 ESP32-S3（N16R8：16 MB flash + 8 MB octal PSRAM）；
# 分区表见 partitions.csv（app 3 MB + model 6 MB，model 是 esp-sr 的模型分区）。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lamp_av)
