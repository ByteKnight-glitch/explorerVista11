@echo off
echo ===================================================
echo Launching Open-Shell in Vista/Win7 Compatibility Mode
echo ===================================================
taskkill /f /im StartMenu.exe >nul 2>&1
timeout /t 1 /nobreak >nul

if exist "C:\Program Files\Open-Shell\StartMenu.exe" (
    set __COMPAT_LAYER=Win7RTM
    start "" "C:\Program Files\Open-Shell\StartMenu.exe"
    echo Open-Shell started with __COMPAT_LAYER=Win7RTM.
) else (
    echo [ERROR] Open-Shell not found at "C:\Program Files\Open-Shell\StartMenu.exe".
)
