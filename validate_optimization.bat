@echo off
REM ============================================================================
REM VIB Performance Validation Script
REM Validates that Threadripper PRO + RTX 5080 optimizations are working
REM ============================================================================

setlocal enabledelayedexpansion
title VIB Performance Validation

echo ============================================================================
echo          VIB PERFORMANCE VALIDATION - Threadripper PRO + RTX 5080
echo ============================================================================
echo.

set PASS_COUNT=0
set FAIL_COUNT=0
set WARN_COUNT=0

REM ============================================================================
REM Check 1: Hardware Accelerated GPU Scheduling (HAGS)
REM ============================================================================
echo [CHECK 1] Hardware Accelerated GPU Scheduling...

reg query "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" /v HwSchMode >nul 2>&1
if %errorlevel% NEQ 0 (
    echo   [FAIL] HwSchMode registry key not found
    echo   Action: Enable HAGS in Windows Graphics Settings
    set /a FAIL_COUNT+=1
) else (
    for /f "tokens=3" %%a in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" /v HwSchMode ^| findstr HwSchMode') do set HAGS_VALUE=%%a
    
    if "!HAGS_VALUE!"=="0x2" (
        echo   [PASS] HAGS is enabled
        set /a PASS_COUNT+=1
    ) else (
        echo   [FAIL] HAGS is disabled ^(value: !HAGS_VALUE!^)
        echo   Action: Set to 0x2 or enable in Windows Settings
        set /a FAIL_COUNT+=1
    )
)

echo.

REM ============================================================================
REM Check 2: GPU Detection (RTX 5080)
REM ============================================================================
echo [CHECK 2] GPU Detection...

nvidia-smi --query-gpu=name --format=csv,noheader >nul 2>&1
if %errorlevel% NEQ 0 (
    echo   [FAIL] nvidia-smi not available - Cannot verify GPU
    set /a FAIL_COUNT+=1
) else (
    for /f "tokens=*" %%a in ('nvidia-smi --query-gpu^=name --format^=csv^,noheader') do set GPU_NAME=%%a
    
    echo !GPU_NAME! | findstr /C:"RTX 50" >nul
    if !errorlevel!==0 (
        echo   [PASS] RTX 50-series detected: !GPU_NAME!
        set /a PASS_COUNT+=1
    ) else (
        echo   [WARN] GPU is not RTX 50-series: !GPU_NAME!
        echo   Note: Optimizations designed for RTX 5080
        set /a WARN_COUNT+=1
    )
)

echo.

REM ============================================================================
REM Check 3: PCIe Link Width
REM ============================================================================
echo [CHECK 3] PCIe Link Width...

for /f "tokens=*" %%a in ('nvidia-smi --query-gpu^=pcie.link.width.current --format^=csv^,noheader^,nounits 2^>nul') do set PCIE_WIDTH=%%a

if defined PCIE_WIDTH (
    if "!PCIE_WIDTH!"=="16" (
        echo   [PASS] PCIe x16 detected
        set /a PASS_COUNT+=1
    ) else (
        echo   [FAIL] PCIe x!PCIE_WIDTH! detected ^(expected x16^)
        echo   Action: Move GPU to primary PCIe slot
        set /a FAIL_COUNT+=1
    )
) else (
    echo   [FAIL] Cannot determine PCIe link width
    set /a FAIL_COUNT+=1
)

echo.

REM ============================================================================
REM Check 4: VRAM Availability (>12GB free)
REM ============================================================================
echo [CHECK 4] VRAM Availability...

for /f "tokens=*" %%a in ('nvidia-smi --query-gpu^=memory.free --format^=csv^,noheader^,nounits 2^>nul') do set VRAM_FREE_MB=%%a

if defined VRAM_FREE_MB (
    set /a VRAM_FREE_GB=!VRAM_FREE_MB! / 1024
    
    if !VRAM_FREE_GB! GEQ 12 (
        echo   [PASS] !VRAM_FREE_GB! GB VRAM free ^(>12GB available^)
        set /a PASS_COUNT+=1
    ) else (
        echo   [WARN] !VRAM_FREE_GB! GB VRAM free ^(^<12GB^)
        echo   Note: Close other GPU applications for optimal performance
        set /a WARN_COUNT+=1
    )
) else (
    echo   [FAIL] Cannot determine VRAM availability
    set /a FAIL_COUNT+=1
)

echo.

REM ============================================================================
REM Check 5: NDI SDK Installation
REM ============================================================================
echo [CHECK 5] NDI SDK 6 Installation...

if exist "C:\Program Files\NDI\NDI 6 SDK\Include\Processing.NDI.Lib.h" (
    echo   [PASS] NDI SDK 6 found at standard location
    set /a PASS_COUNT+=1
) else (
    echo   [FAIL] NDI SDK 6 not found
    echo   Action: Install NDI SDK 6 from https://ndi.tv/sdk/
    set /a FAIL_COUNT+=1
)

echo.

REM ============================================================================
REM Check 6: CPU Core Count (Threadripper PRO)
REM ============================================================================
echo [CHECK 6] CPU Core Count...

for /f "tokens=*" %%a in ('wmic cpu get NumberOfLogicalProcessors /value ^| findstr "="') do set %%a

if defined NumberOfLogicalProcessors (
    if !NumberOfLogicalProcessors! GEQ 32 (
        echo   [PASS] !NumberOfLogicalProcessors! logical processors detected ^(Threadripper PRO^)
        set /a PASS_COUNT+=1
    ) else (
        echo   [WARN] !NumberOfLogicalProcessors! logical processors ^(expected 32+ for Threadripper PRO 9955WX^)
        echo   Note: CPU affinity optimizations assume 16+ cores
        set /a WARN_COUNT+=1
    )
) else (
    echo   [FAIL] Cannot determine CPU core count
    set /a FAIL_COUNT+=1
)

echo.

REM ============================================================================
REM Check 7: VIB Configuration File
REM ============================================================================
echo [CHECK 7] VIB Configuration...

if exist "config.json" (
    findstr /C:"performance_optimization" config.json >nul
    if !errorlevel!==0 (
        echo   [PASS] config.json has performance_optimization section
        set /a PASS_COUNT+=1
    ) else (
        echo   [WARN] config.json missing performance_optimization section
        echo   Note: Using default optimization settings
        set /a WARN_COUNT+=1
    )
) else (
    echo   [FAIL] config.json not found
    echo   Action: Create config.json from template
    set /a FAIL_COUNT+=1
)

echo.

REM ============================================================================
REM Check 8: VIB Log Files (Performance Data)
REM ============================================================================
echo [CHECK 8] VIB Performance Logs...

if not exist "logs\" (
    echo   [WARN] logs\ directory not found
    echo   Note: Run VIB at least once to generate logs
    set /a WARN_COUNT+=1
    goto SKIP_LOG_CHECK
)

for /f "delims=" %%f in ('dir /b /od logs\vib_*.log 2^>nul') do set LATEST_LOG=%%f

if defined LATEST_LOG (
    echo   [INFO] Latest log: logs\!LATEST_LOG!
    
    REM Check for optimization markers
    findstr /C:"Thread affinity set" logs\!LATEST_LOG! >nul
    if !errorlevel!==0 (
        echo   [PASS] CPU affinity optimization active
        set /a PASS_COUNT+=1
    ) else (
        echo   [WARN] CPU affinity not detected in logs
        set /a WARN_COUNT+=1
    )
    
    REM Check for performance data
    findstr /C:"[PERF]" logs\!LATEST_LOG! >nul
    if !errorlevel!==0 (
        echo   [INFO] Performance telemetry found
        
        REM Get last performance line
        for /f "tokens=*" %%a in ('findstr /C:"[PERF]" logs\!LATEST_LOG! ^| find /v ""') do set LAST_PERF=%%a
        
        REM Extract Total time if available
        echo !LAST_PERF! | findstr /C:"Total:" >nul
        if !errorlevel!==0 (
            for /f "tokens=10 delims=: " %%t in ("!LAST_PERF!") do (
                set TOTAL_TIME_STR=%%t
                set TOTAL_TIME=!TOTAL_TIME_STR:ms=!
            )
            
            REM Check if under 32ms budget
            REM Note: Batch script doesn't handle floating point, so we check first two digits
            for /f "tokens=1 delims=." %%a in ("!TOTAL_TIME!") do set TOTAL_INT=%%a
            
            if defined TOTAL_INT (
                if !TOTAL_INT! LEQ 32 (
                    echo   [PASS] Frame time: !TOTAL_TIME_STR! ^(under 32ms budget^)
                    set /a PASS_COUNT+=1
                ) else (
                    echo   [WARN] Frame time: !TOTAL_TIME_STR! ^(exceeds 32ms budget^)
                    echo   Note: System may need further tuning
                    set /a WARN_COUNT+=1
                )
            )
        )
    ) else (
        echo   [WARN] No performance data in latest log
        set /a WARN_COUNT+=1
    )
) else (
    echo   [WARN] No VIB log files found
    echo   Note: Run VIB at least once to generate logs
    set /a WARN_COUNT+=1
)

:SKIP_LOG_CHECK
echo.

REM ============================================================================
REM Summary
REM ============================================================================
echo ============================================================================
echo                            VALIDATION SUMMARY
echo ============================================================================
echo.
echo   PASS: %PASS_COUNT%
echo   WARN: %WARN_COUNT%
echo   FAIL: %FAIL_COUNT%
echo.

if %FAIL_COUNT% GTR 0 (
    echo [RESULT] FAILED - System not ready for optimal performance
    echo.
    echo Action Required:
    echo 1. Fix all FAIL items listed above
    echo 2. Review OPTIMIZATION_GUIDE.md for detailed instructions
    echo 3. Re-run this validation script
    echo.
    exit /b 1
) else if %WARN_COUNT% GTR 0 (
    echo [RESULT] PASSED WITH WARNINGS - System can run but may not reach optimal performance
    echo.
    echo Recommendations:
    echo 1. Review WARN items above
    echo 2. Consult OPTIMIZATION_GUIDE.md for tuning tips
    echo.
    exit /b 0
) else (
    echo [RESULT] PASSED - System is optimally configured for VIB
    echo.
    echo   All checks passed!
    echo   System ready for high-performance operation.
    echo   Expected performance: Cap ^<15ms, Sel ^<5ms, NDI ^<10ms, Total ^<32ms
    echo.
    exit /b 0
)
