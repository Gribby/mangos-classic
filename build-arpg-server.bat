@echo off
rem Rebuild the ARPG world server and put the new mangosd.exe in the run folder, then start it.
rem   build-arpg-server.bat        rebuild, copy, start
rem   build-arpg-server.bat pull   git pull first, then the same
rem
rem Close the ARPG mangosd window first: Windows will not overwrite a running exe.
rem RUN_DIR is the folder you run mangosd from (where mangosd.conf lives); edit it below.
setlocal
cd /d "%~dp0"
set "RUN_DIR=D:\Wow\mangos-arpg\run"
set "BUILT=%~dp0build\bin\x64_Release\mangosd.exe"

if not exist "%RUN_DIR%\mangosd.conf" (
    echo No mangosd.conf in %RUN_DIR%
    echo Edit RUN_DIR near the top of build-arpg-server.bat.
    goto :fail
)

if /i "%~1"=="pull" (
    echo Pulling the latest code...
    git pull || goto :fail
)

rem Re-run the configure step: new source files are only picked up by it.
cmake -S . -B build >nul || goto :fail
echo Building mangosd...
cmake --build build --config Release --target mangosd || goto :fail

copy /y "%BUILT%" "%RUN_DIR%\" >nul || (
    echo Could not copy mangosd.exe. Is the ARPG mangosd still running?
    goto :fail
)
echo Copied the new mangosd.exe to %RUN_DIR%.

cd /d "%RUN_DIR%"
start "ARPG mangosd" mangosd.exe
goto :eof

:fail
echo.
echo Something went wrong; see above.
pause
exit /b 1
