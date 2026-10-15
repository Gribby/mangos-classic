@echo off
rem Send the ARPG server's play statistics (arpg-stats\ in RUN_DIR, written by mangosd: see
rem src\game\Arpg\ArpgStats.h) to this repository's arpg-stats branch, where they are read to tune
rem the game. update-arpg-server.bat runs this; running it by hand is safe too. It keeps its own
rem clone of that branch in .arpg-stats (git ignores it), so your checkout is never touched.
rem Run by hand (double-clicked), it waits for a key at the end so you can read what it did.
setlocal
cd /d "%~dp0"
set "PAUSE_AT_END=1"
if /i "%~1"=="fromupdate" set "PAUSE_AT_END="
if not exist "%~dp0arpg-local.bat" (
    echo No arpg-local.bat here, so the run folder is unknown. Run update-arpg-server.bat once to make it.
    goto :done
)
call "%~dp0arpg-local.bat"
echo Looking for play statistics in %RUN_DIR%\arpg-stats
if not exist "%RUN_DIR%\arpg-stats\*.jsonl" (
    echo None there. The server writes them into the folder it runs from, with Arpg.Stats = 1 in
    echo mangosd.conf. If you start mangosd some other way than run-arpg-server.bat, look for an
    echo arpg-stats folder next to that mangosd.exe.
    goto :done
)
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
rem Its own author, so a PC with no git name or email set still sends.
git diff --cached --quiet || git -c user.name="ARPG stats" -c user.email="arpg-stats@users.noreply.github.com" commit -q -m "Play statistics %DATE% %TIME%" || goto :failpop
rem Pushed every time, so a commit an earlier failed push left behind goes now.
git push origin arpg-stats || goto :failpop
popd
echo Sent the play statistics to the arpg-stats branch.
goto :done

:failpop
popd
:fail
echo Could not send the play statistics; see above.

:done
if defined PAUSE_AT_END pause
exit /b 0
