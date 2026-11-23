@echo off
echo ===================================================
echo Switching Shell to Windows Vista Explorer
echo ===================================================
echo 1. Applying Vista Registry Settings...
reg import "%~dp0VistaShellConfig.reg" >nul 2>&1
reg import "%~dp0Import Me.reg" >nul 2>&1

echo 2. Stopping Open-Shell and existing Explorer process...
taskkill /f /im StartMenu.exe >nul 2>&1
taskkill /f /im explorer.exe >nul 2>&1
timeout /t 1 /nobreak >nul

echo 3. Starting Windows Vista Explorer Shell...
start "" "%~dp0explorer.exe"

echo.
echo Vista Explorer shell started!
echo If you also want Open-Shell, wait 2 seconds and run StartOpenShellForVista.bat
echo (Run RestoreWindows11Shell.bat at any time to switch back).
