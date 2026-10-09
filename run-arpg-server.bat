@echo off
rem Start the ARPG world server (and realmd, if arpg-local.bat names it and it is not running).
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

if defined REALMD_DIR (
    tasklist /fi "imagename eq realmd.exe" | find /i "realmd.exe" >nul || (
        echo Starting realmd...
        start "realmd" /d "%REALMD_DIR%" realmd.exe
    )
)
echo Starting the ARPG mangosd...
start "ARPG mangosd" /d "%RUN_DIR%" mangosd.exe
goto :eof

:fail
echo.
echo Something went wrong; see above.
pause
exit /b 1
