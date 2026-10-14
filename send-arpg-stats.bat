@echo off
rem Send the ARPG server's play statistics (arpg-stats\ in RUN_DIR, written by mangosd: see
rem src\game\Arpg\ArpgStats.h) to this repository's arpg-stats branch, where they are read to tune
rem the game. update-arpg-server.bat runs this; running it by hand is safe too. It keeps its own
rem clone of that branch in .arpg-stats (git ignores it), so your checkout is never touched.
setlocal
cd /d "%~dp0"
if not exist "%~dp0arpg-local.bat" goto :eof
call "%~dp0arpg-local.bat"
if not exist "%RUN_DIR%\arpg-stats\*.jsonl" goto :eof
set "STATS=%~dp0.arpg-stats"
if exist "%STATS%\.git" goto :have
for /f "delims=" %%U in ('git remote get-url origin') do set "ORIGIN=%%U"
git clone -q --single-branch --branch arpg-stats "%ORIGIN%" "%STATS%" || goto :fail
:have
pushd "%STATS%"
git pull -q --rebase origin arpg-stats
if not exist stats mkdir stats
copy /y "%RUN_DIR%\arpg-stats\*.jsonl" stats\ >nul
git add stats
git diff --cached --quiet && goto :nothing
git commit -q -m "Play statistics %DATE% %TIME%" || goto :failpop
git push -q origin arpg-stats || goto :failpop
popd
echo Sent the play statistics to the arpg-stats branch.
goto :eof

:nothing
popd
echo No new play statistics to send.
goto :eof

:failpop
popd
:fail
echo Could not send the play statistics; the update goes on.
exit /b 0
