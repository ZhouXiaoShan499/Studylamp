@echo off
rem ===========================================================================
rem  LAMP-1 语音服务一键安装脚本
rem  做四件事：
rem    1. 用 Python 3.10 创建独立虚拟环境 venv
rem    2. 安装依赖（vosk / websockets / requests）
rem    3. 注册 Windows 任务计划：开机自动运行、无需登录桌面
rem    4. 打印验证命令
rem  需要管理员权限（注册任务计划 + 防火墙放行）。
rem ===========================================================================
setlocal

set "BASE=%~dp0"
cd /d "%BASE%"

echo.
echo ============================================================
echo   LAMP-1 语音服务安装
echo ============================================================
echo.

rem ---- 1. 创建虚拟环境 ------------------------------------------------
set "PY=py"
if not exist "%BASE%venv\Scripts\python.exe" (
    echo [1/4] 创建虚拟环境 venv（Python 3.10）...
    "%PY%" -3.10 -m venv "%BASE%venv"
    if errorlevel 1 (
        echo [ERROR] 创建 venv 失败，请确认已安装 Python 3.10，或改用：
        echo         py -3.10 -m venv "%BASE%venv"
        pause
        exit /b 1
    )
) else (
    echo [1/4] 虚拟环境已存在，跳过。
)

set "VPY=%BASE%venv\Scripts\python.exe"

rem ---- 2. 安装依赖 ------------------------------------------------
echo [2/4] 安装 Python 依赖（vosk / websockets / requests）...
"%VPY%" -m pip install --upgrade pip >nul 2>&1
"%VPY%" -m pip install -r "%BASE%requirements.txt"
if errorlevel 1 (
    echo [ERROR] 依赖安装失败，请检查网络后重试。
    pause
    exit /b 1
)

rem ---- 3. 注册开机自启任务 ------------------------------------------------
set "TASK_NAME=LAMP Voice Service"
set "BAT=%BASE%run_voice_service.bat"

echo [3/4] 注册开机自启任务：%TASK_NAME% ...

rem 先删除可能存在的旧任务（忽略错误）
schtasks /delete /tn "%TASK_NAME%" /f >nul 2>&1

schtasks /create /tn "%TASK_NAME%" /tr "\"%BAT%\"" /sc onstart /ru SYSTEM /rl highest /f
if errorlevel 1 (
    echo [ERROR] 注册任务计划失败，请以管理员身份重新运行本脚本。
    pause
    exit /b 1
)

rem ---- 4. 防火墙放行（ESP32 通过局域网 8765 访问）----------------------
echo [4/4] 放行防火墙 8765 端口...
netsh advfirewall firewall add rule name="LAMP voice" dir=in action=allow protocol=TCP localport=8765 >nul 2>&1

echo.
echo ============================================================
echo   安装完成！
echo   现在可以立即启动一次验证：
echo       schtasks /run /tn "LAMP Voice Service"
echo   验证自检（Ollama + Vosk）：
echo       "%VPY%" "%BASE%voice_server.py" --check
echo   日志位置：
echo       "%BASE%logs\voice_service.log"
echo   以后每次开机，语音服务都会自动在后台运行，无需手动启动。
echo ============================================================
echo.
pause
