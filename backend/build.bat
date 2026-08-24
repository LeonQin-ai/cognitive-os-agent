@echo off
REM c-agent build script (Windows cmd) - uses bundled portable zig
setlocal
cd /d %~dp0

set "ZIG="
if exist "tools\zig\zig.exe" set "ZIG=tools\zig\zig.exe"
if "%ZIG%"=="" (
  where zig >nul 2>nul
  if %errorlevel%==0 (set "ZIG=zig") else (
    echo ERROR: zig not found. Download it to tools\ ^(see README^) or install zig into PATH.
    exit /b 1
  )
)

set "CC=%ZIG% cc"
set "CFLAGS=-std=c11 -Wall -Wextra -O1 -g -Iinclude -Ithird_party/cJSON"
set "LIBS=-lws2_32 -lm"
if not exist build mkdir build

set "SRCS="
for /r src %%f in (*.c) do call set "SRCS=%%SRCS%% %%f"
for /r third_party\cJSON %%f in (*.c) do call set "SRCS=%%SRCS%% %%f"
set "SRCS=%SRCS% third_party\wasm3\wasm3_all.c"

echo [build] cagent
%CC% %CFLAGS% -o build\cagent.exe %SRCS% cli\main.c %LIBS%
if errorlevel 1 exit /b 1
echo [build] cagent-test
%CC% %CFLAGS% -o build\cagent-test.exe %SRCS% tests\test_all.c %LIBS%
if errorlevel 1 exit /b 1
echo [build] mock-llm-server
%CC% %CFLAGS% -o build\mock-llm-server.exe %SRCS% tools\mock_llm_server.c %LIBS%
if errorlevel 1 exit /b 1
echo [build] test-adapters
%CC% %CFLAGS% -o build\test-adapters.exe %SRCS% tests\test_adapters.c %LIBS%
if errorlevel 1 exit /b 1
echo [build] cagent-e2e
%CC% %CFLAGS% -o build\cagent-e2e.exe %SRCS% tests\test_e2e.c %LIBS%
if errorlevel 1 exit /b 1
echo [build] cagent-bench
%CC% %CFLAGS% -o build\cagent-bench.exe %SRCS% tests\bench_agent.c %LIBS%
if errorlevel 1 exit /b 1
echo [done]
