@echo off
REM GPU Memory Leak Fix - Validation Script
REM This script helps validate that the buffer pool fix is working correctly

echo ========================================
echo GPU Memory Leak Fix - Validation
echo ========================================
echo.

echo [1] Checking for compilation errors...
msbuild src\VIB.vcxproj /p:Configuration=Release /t:Build /fl /flp:logfile=build_validation.log;verbosity=minimal
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Compilation failed! Check build_validation.log for details.
    exit /b 1
)
echo [OK] Compilation successful!
echo.

echo [2] Expected log patterns after running VIB.exe:
echo.
echo Initial allocation phase (first 2 seconds):
echo    [INFO] DeckLinkCudaBufferAllocator: Allocated 4147200 bytes (Total: 1/5)
echo    [INFO] DeckLinkCudaBufferAllocator: Allocated 4147200 bytes (Total: 2/5)
echo    [INFO] DeckLinkCudaBufferAllocator: Allocated 4147200 bytes (Total: 3/5)
echo    [INFO] DeckLinkCudaBufferAllocator: Allocated 4147200 bytes (Total: 4/5)
echo    [INFO] DeckLinkCudaBufferAllocator: Allocated 4147200 bytes (Total: 5/5)
echo.
echo Steady state (after 2 seconds - should only see reuse messages):
echo    [INFO] Buffer pool reused 100 times (Pool size: 3/5)
echo    [INFO] Buffer pool reused 200 times (Pool size: 2/5)
echo    [INFO] Buffer returned to pool. 3/5 buffers free. Pool hits: 256
echo.
echo [3] What to check in Task Manager:
echo    - Open Task Manager -^> Performance -^> GPU
echo    - GPU Memory should stabilize at ~20 MB (5 buffers × 4 MB)
echo    - Shared Memory should stay under 100 MB
echo    - 3D GPU usage should drop from 98%% to 30-40%%
echo.
echo [4] Red flags (if you see these, report them):
echo    - New "Allocated" messages after the first 5
echo    - GPU Memory continuing to grow beyond 30 MB
echo    - Shared Memory growing beyond 500 MB
echo    - Warning messages about "Buffer pool exhausted"
echo.

echo ========================================
echo To run VIB and monitor:
echo    1. Start Task Manager (Performance -^> GPU)
echo    2. Run: Release\VIB.exe
echo    3. Watch logs and Task Manager for 5 minutes
echo    4. Memory should stay stable
echo ========================================
