@echo off
REM ============================================================================
REM VIB Real-Time Monitoring Script
REM Displays: VRAM, GPU Temp, Frame Time, Active Cameras
REM Press Ctrl+C to exit
REM ============================================================================

title VIB System Monitor

:LOOP
cls
echo ============================================================================
echo                    VIB SYSTEM MONITOR
echo ============================================================================
echo.

REM Get timestamp
for /f "tokens=1-3 delims=:." %%a in ("%time%") do (
    set HH=%%a
    set MM=%%b
    set SS=%%c
)

REM Remove leading space from HH if present
set HH=%HH: =0%

echo [%HH%:%MM%:%SS%]
echo.

REM ============================================================================
REM GPU Metrics via nvidia-smi
REM ============================================================================
echo --- GPU STATUS ---
for /f "tokens=1-3" %%a in ('nvidia-smi --query-gpu^=memory.used^,temperature.gpu^,utilization.gpu --format^=csv^,noheader^,nounits 2^>nul') do (
    set VRAM_MB=%%a
    set GPU_TEMP=%%b
    set GPU_UTIL=%%c
)

REM Convert MB to GB with precision
if defined VRAM_MB (
    set /a VRAM_GB_INT=%VRAM_MB% / 1024
    set /a VRAM_GB_DEC=(%VRAM_MB% * 100 / 1024) %% 100
    if %VRAM_GB_DEC% LSS 10 set VRAM_GB_DEC=0%VRAM_GB_DEC%
    echo VRAM:   %VRAM_GB_INT%.%VRAM_GB_DEC% GB ^(%VRAM_MB% MB^)
    echo Temp:   %GPU_TEMP%C
    echo GPU:    %GPU_UTIL%%%
) else (
    echo VRAM:   [nvidia-smi not available]
    echo Temp:   --
    echo GPU:    --
)

echo.

REM ============================================================================
REM VIB Performance Metrics from logs
REM ============================================================================
echo --- VIB PERFORMANCE ---

REM Find most recent log file
for /f "delims=" %%f in ('dir /b /od logs\vib_*.log 2^>nul') do set LATEST_LOG=%%f

if defined LATEST_LOG (
    REM Extract last [PERF] line from log
    for /f "tokens=*" %%a in ('findstr /C:"[PERF]" logs\%LATEST_LOG% 2^>nul ^| find /v "" ^| more +0') do set LAST_PERF=%%a
    
    if defined LAST_PERF (
        REM Parse frame time from "Total:XX.Xms"
        for /f "tokens=6 delims=: " %%t in ("!LAST_PERF!") do (
            set FRAME_TIME=%%t
        )
        
        REM Parse active cameras from "Active:X/12"
        for /f "tokens=7 delims=: " %%a in ("!LAST_PERF!") do (
            set ACTIVE_CAMS=%%a
        )
        
        if defined FRAME_TIME (
            echo Frame:  !FRAME_TIME!
        ) else (
            echo Frame:  --ms
        )
        
        if defined ACTIVE_CAMS (
            echo Active: !ACTIVE_CAMS!
        ) else (
            echo Active: --/12
        )
        
        echo.
        echo Last Perf: !LAST_PERF!
    ) else (
        echo Frame:  [No performance data yet]
        echo Active: --/12
    )
) else (
    echo Frame:  [No log files found]
    echo Active: --/12
    echo.
    echo Log folder: logs\
)

echo.
echo ============================================================================
echo Refreshing every 2 seconds... Press Ctrl+C to exit
echo ============================================================================

REM Wait 2 seconds before next update
timeout /t 2 /nobreak >nul

goto LOOP
