@echo off
rem ===========================================================================
rem  LAMP-1 语音服务运行脚本（被任务计划调用，也可手动双击启动）
rem  职责：激活虚拟环境 -> 启动 voice_server.py，进程异常退出自动重启。
rem  日志写入 server\logs\voice_service.log，便于排查。
rem ===========================================================================
setlocal

set "BASE=%~dp0"
cd /d "%BASE%"

rem 虚拟环境尚未创建时，提示先运行 install_service.bat
if not exist "%BASE%venv\Scripts\python.exe" (
    echo [ERROR] 未找到虚拟环境，请先双击运行 install_service.bat
    pause
    exit /b 1
)

set "PY=%BASE%venv\Scripts\python.exe"

rem 日志目录
if not exist "%BASE%logs" mkdir "%BASE%logs"
set "LOG=%BASE%logs\voice_service.log"

rem 默认模型 4b，可通过环境变量 LAMP_MODEL 覆盖
if "%LAMP_MODEL%"=="" set "LAMP_MODEL=qwen3.5:4b"

echo ==============================================================>> "%LOG%"
echo [%date% %time%] 启动 voice_server.py (model=%LAMP_MODEL%)>> "%LOG%"

rem 循环启动：服务退出后 3 秒自动重启，避免崩溃后一直躺着
:loop
echo [%date% %time%] 进程启动中...>> "%LOG%"
"%PY%" "%BASE%voice_server.py" --model "%LAMP_MODEL%" >> "%LOG%" 2>&1
set "RC=%ERRORLEVEL%"
echo [%date% %time%] 进程退出，code=%RC%，3 秒后重启...>> "%LOG%"
timeout /t 3 /nobreak >nul
goto loop
