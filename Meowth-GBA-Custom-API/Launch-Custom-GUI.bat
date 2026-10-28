@echo off
setlocal
cd /d "%~dp0"
title Meowth - Custom API GUI
if not exist "pyproject.toml" goto FAILED
if not exist ".venv\Scripts\python.exe" (
  py -3 -m venv ".venv"
  if errorlevel 1 (
    python -m venv ".venv"
    if errorlevel 1 goto FAILED
  )
)
".venv\Scripts\python.exe" -m pip install --disable-pip-version-check -e ".[gui]"
if errorlevel 1 goto FAILED
".venv\Scripts\python.exe" -c "from meowth.gui import main; main()"
if errorlevel 1 goto FAILED
exit /b 0
:FAILED
echo ERROR: Python 3.10+ or Meowth source is required.
pause
exit /b 1
