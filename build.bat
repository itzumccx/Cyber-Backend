@echo off
title Cyber Build
echo =================================
echo   Cyber - Build
echo =================================

REM Check if gcc is available
where gcc >nul 2>&1
if %errorlevel% neq 0 (
    echo [Error] gcc not found. Install MinGW and add it to PATH.
    echo Download: https://www.mingw-w64.org/
    pause
    exit /b 1
)

echo [Build] Compiling...

gcc -Wall -O2 ^
    -Ivendor/cjson ^
    -Ivendor/mongoose ^
    -DMG_ENABLE_LOG=0 ^
    src/main.c ^
    src/router.c ^
    src/auth.c ^
    src/eos.c ^
    src/mcp.c ^
    src/friends.c ^
    src/locker.c ^
    src/shop.c ^
    src/cloudstorage.c ^
    src/profile.c ^
    src/log.c ^
    src/errors.c ^
    src/version.c ^
    vendor/cjson/cJSON.c ^
    vendor/mongoose/mongoose.c ^
    -o cyber.exe ^
    -lws2_32

if %errorlevel% neq 0 (
    echo [Error] Build failed.
    pause
    exit /b 1
)

echo [Build] Success: cyber.exe created.
