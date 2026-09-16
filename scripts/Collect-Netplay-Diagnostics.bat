@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Collect-Netplay-Diagnostics.ps1"
if errorlevel 1 (
  echo.
  echo Failed to create Netplay diagnostic package.
  pause
)
endlocal
