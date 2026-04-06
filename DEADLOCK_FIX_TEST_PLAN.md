# VIB Deadlock Fix - Test Plan

## Objective
Verify that the recursive mutex fix resolves the RESOURCE_DEADLOCK_WOULD_OCCUR exception and allows VIB.exe to run with all 12 NDI channels operational.

## Prerequisites
- Windows 10/11 with Visual Studio 2022
- NVIDIA RTX 5080 (or compatible GPU with CUDA 13.2)
- NDI SDK 6 installed at `C:\Program Files\NDI\NDI 6 SDK`
- DeckLink 4K Extreme 12G capture card (4-channel mode)
- vMix or other NDI receiver for verification

## Test Cases

### TC1: Build Verification
**Objective**: Ensure code compiles without errors

**Steps**:
1. Open `src\VIB.sln` in Visual Studio 2022
2. Select Configuration: `Release`, Platform: `x64`
3. Build → Rebuild Solution
4. Check Output window for errors

**Expected Result**:
- ✅ Build succeeds with 0 errors
- ✅ VIB.exe created in `src\x64\Release\`
- ⚠️ Warnings (if any) should not be related to mutex usage

**Pass Criteria**: Build completes successfully

---

### TC2: Application Startup
**Objective**: Verify no deadlock on initialization

**Steps**:
1. Navigate to `src\x64\Release\`
2. Run `VIB.exe`
3. Observe console output for first 10 seconds
4. Check for any exception dialogs

**Expected Result**:
- ✅ Application starts without RESOURCE_DEADLOCK_WOULD_OCCUR
- ✅ Console shows initialization messages:
  - "NDIManager: Librería NDI 6 inicializada correctamente"
  - "ActiveCameraSelector initialized"
  - "Selector warming up... (X/10 frames)"
- ✅ No crash or hang
- ✅ No Windows exception dialog

**Pass Criteria**: Application runs for at least 30 seconds without exceptions

---

### TC3: 12-Channel Processing
**Objective**: Verify all 12 cameras process frames without deadlock

**Steps**:
1. With VIB.exe running, monitor console output
2. Look for frame processing logs
3. Let run for 5 minutes
4. Check Windows Task Manager → Performance → GPU for activity

**Expected Result**:
- ✅ All 12 channels show frame callbacks
- ✅ Console logs motion scores for channels 0-11
- ✅ GPU utilization: 20-40% (expected for 12x4K channels)
- ✅ VRAM usage: ~1.6-1.65GB (stable, no leak)
- ✅ No "Camera X deadlock" or timeout messages

**Pass Criteria**: All 12 channels process continuously for 5 minutes

---

### TC4: NDI Output Verification
**Objective**: Confirm all 12 NDI sources are visible and streaming

**Steps**:
1. With VIB.exe running, open vMix (or NDI Studio Monitor)
2. Click "Add Input" → NDI
3. Check available NDI sources list
4. Add VIB_CAM_01 through VIB_CAM_12 as inputs
5. Observe video preview for each

**Expected Result**:
- ✅ All 12 sources appear in NDI source list:
  - VIB_CAM_01, VIB_CAM_02, ..., VIB_CAM_12
- ✅ Each source shows live video (not black screen)
- ✅ Frame rate: ~29.97 fps (check vMix input settings)
- ✅ No "Source Lost" or connection drops

**Pass Criteria**: All 12 NDI sources stream continuously for 5 minutes

---

### TC5: Stress Test - High Motion
**Objective**: Verify no deadlock under high camera activity

**Steps**:
1. Set up test scenario with high motion on all cameras
   - Pan cameras, move objects in frame, etc.
2. Run VIB.exe for 30 minutes
3. Monitor console for performance logs every 30 frames
4. Check for any exceptions or hangs

**Expected Result**:
- ✅ Application runs continuously for 30 minutes
- ✅ Performance logs show timing within budget:
  - Total: 12-20ms (< 33ms budget)
  - Cap: 0.3-0.5ms, Sel: 0.3-0.5ms, YOLO: 10-14ms, NDI: 0.6-0.9ms
- ✅ No deadlock exceptions
- ✅ No memory leaks (VRAM stays ~1.6GB)
- ✅ CPU usage: 15-25% (stable)

**Pass Criteria**: 30-minute continuous operation without issues

---

### TC6: Concurrent Camera Updates
**Objective**: Test recursive mutex allows ProcessFrame → AllocateCameraResources

**Steps**:
1. Run VIB.exe with config modified to force resource reallocation
2. Monitor console for "Allocating resources" messages
3. Verify no deadlock when cameras initialize at different times
4. Check all cameras eventually become active

**Expected Result**:
- ✅ Resources allocated for each camera on first frame
- ✅ No RESOURCE_DEADLOCK_WOULD_OCCUR during allocation
- ✅ Console shows: "Selector warming up... (X/10 frames)"
- ✅ Eventually: "Selector stable (10/10 frames)"

**Pass Criteria**: All cameras complete warmup without deadlock

---

### TC7: Monitor.bat Verification
**Objective**: Use monitoring script to track real-time performance

**Steps**:
1. Open separate command prompt
2. Navigate to VIB root directory
3. Run `monitor.bat`
4. Observe real-time dashboard while VIB.exe runs
5. Let run for 10 minutes

**Expected Result**:
- ✅ Dashboard updates every 2 seconds
- ✅ Shows VRAM usage (~1.6GB), GPU temp (<80°C), GPU% (20-40%)
- ✅ Frame timing data parsed from logs
- ✅ Active cameras count: 4 (or as configured)
- ✅ No error messages in monitor output

**Pass Criteria**: Monitor shows stable metrics for 10 minutes

---

### TC8: Emergency Shutdown (F4)
**Objective**: Verify graceful shutdown without deadlock

**Steps**:
1. With VIB.exe running normally
2. Press F4 key (emergency shutdown)
3. Observe shutdown sequence in console
4. Check process exits cleanly

**Expected Result**:
- ✅ Application responds to F4 within 1 second
- ✅ Console shows: "Emergency shutdown triggered..."
- ✅ All resources released (GPU memory freed)
- ✅ Process exits with code 0
- ✅ No "Process still running" in Task Manager

**Pass Criteria**: Clean shutdown within 3 seconds

---

## Regression Tests

### RT1: Existing Functionality
Verify these features still work after mutex changes:
- ✅ Hysteresis prevents camera flickering
- ✅ Adaptive quality (F2/F3 keys)
- ✅ Telemetry logging to JSON
- ✅ Redis integration (if enabled)

### RT2: Performance Baselines
Compare with pre-fix benchmarks (if available):
- Frame processing time: Should be same or better
- NDI latency: Should be same or better (~0.8ms)
- GPU memory: Should be same (~1.6GB)

---

## Pass/Fail Criteria

**PASS**: All test cases (TC1-TC8) pass
**FAIL**: Any test case fails, especially TC2-TC6

**Critical Tests** (must pass):
- TC2: Application Startup (no deadlock)
- TC3: 12-Channel Processing
- TC6: Concurrent Camera Updates

**Important Tests** (should pass):
- TC4: NDI Output Verification
- TC5: Stress Test

**Nice to Have**:
- TC7: Monitor.bat
- TC8: Emergency Shutdown

---

## Bug Reporting

If test fails, capture:
1. Full console output from VIB.exe
2. Windows Event Viewer → Application logs (errors)
3. GPU-Z logs (if GPU-related)
4. Steps to reproduce
5. System specs (GPU model, driver version, OS build)

Submit to: [GitHub Issues or internal tracker]

---

## Sign-Off

| Role | Name | Date | Status |
|------|------|------|--------|
| Developer | [Agent] | 2026-04-06 | ✅ Code Complete |
| QA Engineer | | | Pending |
| Tech Lead | | | Pending |
| Product Owner | | | Pending |

---

## Notes
- This test plan focuses on the deadlock fix and mutex changes
- Performance benchmarks should match DEPLOYMENT_VALIDATION_PLAN.md
- If tests fail, consult DEADLOCK_FIX_SUMMARY.md for rollback steps
