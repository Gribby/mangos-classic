@echo off
rem Pull the latest ARPG server code, rebuild mangosd, put it in the run folder and start it.
rem Close the ARPG mangosd window first: Windows will not overwrite a running exe.
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

echo Pulling the latest code...
git pull || goto :fail
rem Re-run the configure step: new source files are only picked up by it.
cmake -S . -B build >nul || goto :fail
echo Building mangosd (a big change can take a while)...
cmake --build build --config Release --target mangosd || goto :fail
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
