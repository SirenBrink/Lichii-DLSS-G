@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-NvidiaRuntime.ps1"
if errorlevel 1 (
  echo.
  echo Installation did not complete.
  pause
  exit /b 1
)
echo.
echo NVIDIA runtime installed. Restart Lichii to use it.
pause
