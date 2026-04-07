@echo off
REM ============================================================================
REM VIB Real-Time Monitoring Script - Enhanced for Threadripper PRO + RTX 5080
REM Displays: VRAM, GPU Temp, PCIe, Frame Time, Cap/Sel/NDI breakdown
REM Press Ctrl+C to exit
REM ============================================================================

setlocal enabledelayedexpansion
title VIB System Monitor - Threadripper PRO + RTX 5080

:LOOP
cls
echo ============================================================================
echo                    VIB SYSTEM MONITOR (Enhanced)
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
REM GPU Metrics via nvidia-smi (Enhanced)
REM ============================================================================
echo --- GPU STATUS (RTX 5080) ---
for /f "tokens=1-6" %%a in ('nvidia-smi --query-gpu^=memory.used^,memory.total^,temperature.gpu^,utilization.gpu^,utilization.memory^,pcie.link.width.current --format^=csv^,noheader^,nounits 2^>nul') do (
    set VRAM_USED=%%a
    set VRAM_TOTAL=%%b
    set GPU_TEMP=%%c
    set GPU_UTIL=%%d
    set MEM_UTIL=%%e
    set PCIE_WIDTH=%%f
)

REM Display GPU metrics
if defined VRAM_USED (
    set /a VRAM_GB_INT=%VRAM_USED% / 1024
    set /a VRAM_GB_DEC=(%VRAM_USED% * 100 / 1024) %% 100
    set /a VRAM_TOTAL_GB=%VRAM_TOTAL% / 1024
    set /a VRAM_PCT=(%VRAM_USED% * 100) / %VRAM_TOTAL%
    if %VRAM_GB_DEC% LSS 10 set VRAM_GB_DEC=0%VRAM_GB_DEC%
    
    echo VRAM:   %VRAM_GB_INT%.%VRAM_GB_DEC% GB / %VRAM_TOTAL_GB% GB ^(%VRAM_PCT%%%^)
    echo Temp:   %GPU_TEMP%C
    echo GPU:    %GPU_UTIL%%%
    echo Mem:    %MEM_UTIL%%%
    
    REM PCIe status
    if defined PCIE_WIDTH (
        if %PCIE_WIDTH%==16 (
            echo PCIe:   x%PCIE_WIDTH% [OPTIMAL]
        ) else (
            echo PCIe:   x%PCIE_WIDTH% [WARNING: Expected x16]
        )
    ) else (
        echo PCIe:   --
    )
) else (
    echo VRAM:   [nvidia-smi not available]
    echo Temp:   --
    echo GPU:    --
    echo Mem:    --
    echo PCIe:   --
)

echo.

REM ============================================================================
REM VIB Performance Metrics from logs (Enhanced)
REM ============================================================================
echo --- VIB PERFORMANCE (Frame Breakdown) ---

REM Find most recent log file
for /f "delims=" %%f in ('dir /b /od logs\vib_*.log 2^>nul') do set LATEST_LOG=%%f

if defined LATEST_LOG (
    REM Extract last [PERF] line from log
    for /f "tokens=*" %%a in ('findstr /C:"[PERF]" logs\%LATEST_LOG% 2^>nul ^| find /v "" ^| more +0') do set LAST_PERF=%%a
    
    if defined LAST_PERF (
        REM Parse timing values: Cap:X.Xms Sel:X.Xms YOLO:X.Xms NDI:X.Xms Total:X.Xms
        echo !LAST_PERF! | findstr /C:"Cap:" >nul
        if !errorlevel!==0 (
            REM Extract individual timings
            for /f "tokens=2,4,6,8,10,12 delims=: " %%a in ("!LAST_PERF!") do (
                set CAP_TIME=%%a
                set SEL_TIME=%%b
                set YOLO_TIME=%%c
                set NDI_TIME=%%d
                set REDIS_TIME=%%e
                set TOTAL_TIME=%%f
            )
            
            REM Display with color-coded status
            if defined CAP_TIME (
                set CAP_DISPLAY=!CAP_TIME!
                REM Remove 'ms' suffix for comparison
                set CAP_NUM=!CAP_TIME:ms=!
            )
            if defined SEL_TIME set SEL_DISPLAY=!SEL_TIME!
            if defined NDI_TIME set NDI_DISPLAY=!NDI_TIME!
            if defined TOTAL_TIME set TOTAL_DISPLAY=!TOTAL_TIME!
            
            echo Cap:    !CAP_DISPLAY!  ^(target: ^<15ms^)
            echo Sel:    !SEL_DISPLAY!  ^(target: ^<5ms^)
            echo YOLO:   !YOLO_TIME!
            echo NDI:    !NDI_DISPLAY!  ^(target: ^<10ms^)
            echo Redis:  !REDIS_TIME!
            echo --------------------------------
            echo Total:  !TOTAL_DISPLAY!  ^(budget: ^<32ms^)
        ) else (
            echo [Old log format - no timing breakdown]
        )
        
        REM Parse active cameras
        for /f "tokens=7 delims=: " %%a in ("!LAST_PERF!") do (
            set ACTIVE_CAMS=%%a
        )
        
        if defined ACTIVE_CAMS (
            echo Active: !ACTIVE_CAMS! cameras
        ) else (
            echo Active: --/12
        )
        
    ) else (
        echo Cap:    [No performance data yet]
        echo Sel:    --
        echo YOLO:   --
        echo NDI:    --
        echo Total:  --
        echo Active: --/12
    )
) else (
    echo Cap:    [No log files found]
    echo Sel:    --
    echo YOLO:   --
    echo NDI:    --
    echo Total:  --
    echo Active: --/12
    echo.
    echo Log folder: logs\
)

echo.

REM ============================================================================
REM System Optimization Status
REM ============================================================================
echo --- OPTIMIZATION STATUS ---

REM Check for optimization warnings in log
if defined LATEST_LOG (
    findstr /C:"Hardware Accelerated GPU Scheduling: DISABLED" logs\%LATEST_LOG% >nul 2>&1
    if !errorlevel!==0 (
        echo [WARNING] HAGS is DISABLED - Enable in Windows Graphics Settings
    ) else (
        findstr /C:"Hardware Accelerated GPU Scheduling: ENABLED" logs\%LATEST_LOG% >nul 2>&1
        if !errorlevel!==0 (
            echo [OK] Hardware Accelerated GPU Scheduling enabled
        )
    )
    
    findstr /C:"SUBOPTIMAL" logs\%LATEST_LOG% >nul 2>&1
    if !errorlevel!==0 (
        echo [WARNING] PCIe configuration is SUBOPTIMAL - Check logs
    ) else (
        findstr /C:"OPTIMAL" logs\%LATEST_LOG% >nul 2>&1
        if !errorlevel!==0 (
            echo [OK] System configuration is optimal
        )
    )
    
    findstr /C:"Thread affinity set" logs\%LATEST_LOG% >nul 2>&1
    if !errorlevel!==0 (
        echo [OK] CPU core affinity configured
    )
)

echo.
echo ============================================================================
echo Refreshing every 2 seconds... Press Ctrl+C to exit
echo See OPTIMIZATION_GUIDE.md for performance tuning
echo ============================================================================

REM Wait 2 seconds before next update
timeout /t 2 /nobreak >nul

goto LOOP

