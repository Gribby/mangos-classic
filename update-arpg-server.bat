@echo off
rem Pull the latest ARPG server code, re-run CMake, rebuild mangosd, put it in the run folder and
rem start it.
rem   update-arpg-server.bat         pull, configure, build what changed, copy, start
rem   update-arpg-server.bat full    the same, but rebuild everything from clean (after a big
rem                                  update, or when a build acts strangely)
rem   update-arpg-server.bat nopull  skip the git pull (build your own local changes)
rem The ARPG mangosd must not be running while the new exe is copied: if it is, this asks to
rem stop it. Only the mangosd in RUN_DIR is touched, never another server's.
setlocal
cd /d "%~dp0"
rem Load (and on first run create) arpg-local.bat: this machine's paths, which git ignores.
if not exist "%~dp0arpg-local.bat" (
    > "%~dp0arpg-local.bat" echo rem This machine's ARPG server paths. Git ignores this file, so edit it freely.
    >> "%~dp0arpg-local.bat" echo rem RUN_DIR: the folder the ARPG mangosd runs from, where its mangosd.conf lives.
    >> "%~dp0arpg-local.bat" echo set "RUN_DIR=D:\Wow\mangos-arpg\run"
    >> "%~dp0arpg-local.bat" echo rem REALMD_DIR: optional; the folder of the realmd to start if it is not running.
    >> "%~dp0arpg-local.bat" echo rem set "REALMD_DIR=D:\Wow\mangos-classic\run"
    echo Created arpg-local.bat with RUN_DIR=D:\Wow\mangos-arpg\run.
    echo Edit arpg-local.bat if your run folder is elsewhere, or to start realmd too.
)
call "%~dp0arpg-local.bat"
if not exist "%RUN_DIR%\mangosd.conf" (
    echo No mangosd.conf in %RUN_DIR%
    echo Edit RUN_DIR in arpg-local.bat.
    goto :fail
)

set "PULL=1"
set "CLEAN="
for %%A in (%*) do (
    if /i "%%~A"=="full" set "CLEAN=--clean-first"
    if /i "%%~A"=="nopull" set "PULL="
)

rem Stop the ARPG mangosd first (it would lock mangosd.exe), with a yes from you.
set "RUNNING="
for /f %%P in ('powershell -NoProfile -Command "(Get-Process mangosd -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq '%RUN_DIR%\mangosd.exe' }).Id"') do set "RUNNING=%%P"
if defined RUNNING (
    echo The ARPG mangosd is running ^(process %RUNNING%^).
    choice /c YN /m "Stop it now? Unsaved progress from the last few minutes may be lost"
    if errorlevel 2 (
        echo Close it yourself ^(type "server shutdown 0" in its window^), then run this again.
        goto :fail
    )
    taskkill /pid %RUNNING% >nul 2>&1
    timeout /t 3 /nobreak >nul
    taskkill /f /pid %RUNNING% >nul 2>&1
)

rem The stopped server's play statistics go to the arpg-stats branch first (never stops the update).
call "%~dp0send-arpg-stats.bat"

if defined PULL (
    echo Pulling the latest code...
    git pull || goto :fail
)

rem Re-run the configure step: new source files are only picked up by it.
echo Configuring with CMake...
cmake -S . -B build > "%TEMP%\arpg-cmake.log" 2>&1 || (
    type "%TEMP%\arpg-cmake.log"
    goto :fail
)

if defined CLEAN (
    echo Rebuilding mangosd from clean ^(this takes a while^)...
) else (
    echo Building mangosd ^(a big change can take a while^)...
)
cmake --build build --config Release --target mangosd %CLEAN% --parallel || goto :fail

copy /y "build\bin\x64_Release\mangosd.exe" "%RUN_DIR%\" >nul || (
    echo Could not copy mangosd.exe. Is the ARPG mangosd still running?
    goto :fail
)
echo Copied the new mangosd.exe to %RUN_DIR%.
call "%~dp0run-arpg-server.bat"
goto :eof

:fail
echo.
echo Something went wrong; see above.
pause
exit /b 1
