@echo off
rem ---------------------------------------------------------------------------
rem ESP-IDF wrapper. Pins the Python venv so export.bat does not pick up the
rem system Python 3.13 (there is no idf5.4_py3.13_env on this machine).
rem
rem IMPORTANT: keep this file pure ASCII. cmd.exe parses .bat as GBK here, so
rem Chinese comments break the parser and produce errors like "'uild' is not
rem recognized as an internal or external command".
rem
rem Usage (from cmd.exe):
rem   idf.bat --version
rem   idf.bat set-target esp32s3
rem   idf.bat build
rem   idf.bat -p COM5 flash monitor
rem All arguments are passed straight through to idf.py.
rem ---------------------------------------------------------------------------
set "IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.4.3"
set "IDF_PYTHON_ENV_PATH=D:\Espressif\python_env\idf5.4_py3.11_env"

rem If this .bat is launched from Git Bash / MSYS (e.g. "cmd /c idf.bat"),
rem export.bat sees MSYSTEM and refuses: "This .bat file is for Windows
rem CMD.EXE shell only." Clear the traces so it always runs.
set "MSYSTEM="
set "SHELL="
set "TERM="

if not exist "%IDF_PATH%\export.bat" (
    echo [ERROR] IDF not found at %IDF_PATH%
    exit /b 1
)
if not exist "%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" (
    echo [ERROR] venv not found at %IDF_PYTHON_ENV_PATH%
    echo         run: dir D:\Espressif\python_env
    exit /b 1
)

call "%IDF_PATH%\export.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] export.bat failed
    echo         try running it manually to see the real error:
    echo         "%IDF_PATH%\export.bat"
    exit /b 1
)

idf.py %*
