@echo off
rem Proves that nothing was added to, changed in or removed from the
rem "Test Songs" folder beside this file (compared with checks\test-songs-manifest.txt).
setlocal
set "HERE=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%checks\test-songs-manifest.ps1" -Folder "%HERE%Test Songs" -Verify "%HERE%checks\test-songs-manifest.txt"
pause
