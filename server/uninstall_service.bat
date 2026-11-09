@echo off
rem ===========================================================================
rem  LAMP-1 语音服务卸载脚本：删除任务计划 + 防火墙规则（保留 venv 和模型）
rem ===========================================================================
setlocal

set "TASK_NAME=LAMP Voice Service"

echo 正在删除开机自启任务 %TASK_NAME% ...
schtasks /end /tn "%TASK_NAME%" >nul 2>&1
schtasks /delete /tn "%TASK_NAME%" /f >nul 2>&1

echo 正在删除防火墙规则 LAMP voice ...
netsh advfirewall firewall delete rule name="LAMP voice" >nul 2>&1

echo.
echo 卸载完成。虚拟环境和模型文件已保留（如需彻底清理，可删除 server\venv）。
pause
