@echo off
if "%~1"=="" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_collection_test.ps1" -Interactive
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_collection_test.ps1" %*
)
pause
