@echo off
echo ===================================================
echo Restoring Windows 11 Default Explorer Shell
echo ===================================================
echo 1. Terminating running Explorer processes...
taskkill /f /im explorer.exe >nul 2>&1
timeout /t 1 /nobreak >nul

echo 2. Launching native Windows 11 Explorer...
start "" "C:\Windows\explorer.exe"

echo.
echo Native Windows 11 Explorer shell restored!
