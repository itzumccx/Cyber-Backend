@echo off
title Cyber - Fortnite Backend
echo =================================
echo   Cyber - Fortnite Backend
echo =================================

echo [Info] Rebuilding so source changes are in cyber.exe...
call build.bat
if %errorlevel% neq 0 (
    echo [Error] Build failed. Cannot start.
    pause
    exit /b 1
)
title Cyber - Fortnite Backend

echo [Info] Starting Cyber...
echo [Info] Port: 5595 (same as Neonite)
echo [Info] Press Ctrl+C to stop
echo ---------------------------------

cyber.exe

echo.
echo [Info] Cyber stopped.
pause
