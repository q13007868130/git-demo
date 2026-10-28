@echo off
setlocal
cd /d "%~dp0"
title Meowth - Install Custom API GUI Patch

echo [1/3] Checking Meowth source tree...
if not exist "pyproject.toml" goto WRONG_DIR
if not exist "src\meowth\gui\components\config_form.py" goto WRONG_DIR
if not exist "Meowth-GBA-Custom-API\src\meowth\gui\components\config_form.py" goto MISSING_PATCH

set "TARGET=src\meowth\gui\components\config_form.py"
set "BACKUP=src\meowth\gui\components\config_form.py.before_custom_api.bak"

if not exist "%BACKUP%" (
  echo [2/3] Backing up original GUI module...
  copy /y "%TARGET%" "%BACKUP%" >nul
  if errorlevel 1 goto FAILED
)
echo [3/3] Installing modified GUI module...
copy /y "Meowth-GBA-Custom-API\src\meowth\gui\components\config_form.py" "%TARGET%" >nul
if errorlevel 1 goto FAILED

echo SUCCESS: Custom API GUI installed.
pause
exit /b 0

:WRONG_DIR
echo ERROR: Run this in the upstream Meowth source root.
goto FAILED
:MISSING_PATCH
echo ERROR: Place the Meowth-GBA-Custom-API folder inside the upstream source root.
:FAILED
pause
exit /b 1
