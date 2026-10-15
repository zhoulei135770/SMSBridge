@echo off
rem ===========================================================================
rem  app\build_host_tests.bat
rem  sms_pdu 主机端单元测试: 生成向量 -> 编译 -> 运行（一条命令完成）
rem
rem  用法（在仓库根目录）:  app\build_host_tests.bat
rem  退出码: 0 = 全部通过, 非 0 = 有失败或构建失败
rem ===========================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0.."

set CC=gcc
set PYTHON=python
set APPDIR=app
set BUILD=%APPDIR%\build
set TARGET=%BUILD%\test_sms_pdu.exe

echo ==== [1/3] 生成测试向量 (%APPDIR%\test\gen_pdus.py) ====
%PYTHON% %APPDIR%\test\gen_pdus.py --emit-c-header %APPDIR%\test\sms_pdu_vectors.h
if errorlevel 1 (
  echo [FAIL] 向量生成失败
  exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"

echo.
echo ==== [2/3] 编译 (%CC%) ====
%CC% -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I %APPDIR%\inc ^
  %APPDIR%\test\test_sms_pdu.c %APPDIR%\src\sms_pdu.c -o %TARGET% ^
  -fsanitize=address,undefined
if errorlevel 1 (
  echo [FAIL] 编译失败
  exit /b 1
)

echo.
echo ==== [3/3] 运行 ====
"%TARGET%"
set RC=%ERRORLEVEL%
echo.
if %RC%==0 (
  echo ==== 测试通过 ====
) else (
  echo ==== 测试失败 ^(exit code %RC%^) ====
)
exit /b %RC%
