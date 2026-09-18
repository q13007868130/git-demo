@echo off
setlocal
set "PARENT_PID=%~1"
set "TMP_PS1=%TEMP%\PCSX2-Netplay-Updater-%RANDOM%-%RANDOM%.ps1"
copy /Y "%~dp0Update-Netplay.ps1" "%TMP_PS1%" >nul
if errorlevel 1 (
  echo Failed to prepare updater.
  pause
  exit /b 1
)
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%TMP_PS1%" -AppDir "%~dp0" -ParentPid "%PARENT_PID%"
endlocal
