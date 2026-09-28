@echo off
rem Checks that this copy of Frankie's Karaoke Studio has everything it needs,
rem and plays a test song silently. The result opens in Notepad.
setlocal
set "HERE=%~dp0"
echo Checking Frankie's Karaoke Studio... (this takes about 10 seconds)
start "" /wait "%HERE%Program\FrankiesKaraokeStudio.exe" --self-check "%HERE%installation-check.txt" "%HERE%Test Songs\FKT01-01 - Test Singer - Clicks In Time.mp3"
if errorlevel 1 (echo.& echo PROBLEMS FOUND - see the report.) else (echo.& echo All checks passed.)
start "" notepad "%HERE%installation-check.txt"
pause
