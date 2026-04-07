# MegaCanvas/vMix WGC Issue - Resolution

## Problem Statement
User reported that the virtual window "VIB MegaCanvas 16K (vMix WGC)" does not appear in vMix's Windows Graphics Capture (WGC) window selector.

## Root Cause Analysis

### Issue #1: MegaCanvas Feature Not Implemented
The logs reference MegaCanvas functionality that **does not exist** in the current codebase:

**Evidence from logs:**
```
[2026-04-07 16:24:45.579] [INFO] Initializing MegaCanvas (16K Atlas) for vMix WGC...
[2026-04-07 16:24:45.686] [INFO] DXGIPresenter: Borderless window created at (20000,0) size 15360x6480
[2026-04-07 16:24:45.746] [INFO] 2. Select window: 'VIB MegaCanvas 16K (vMix WGC)'
```

**Reality:**
- The repository contains `SpoutManager.cpp` for Spout-based output
- **No** `DXGIPresenter.cpp`, `MegaCanvasManager.cpp`, or related files exist
- Repository memories reference these components, but they are not in the codebase

**Conclusion:** The system is currently using **Spout** for vMix integration, not Windows Graphics Capture. The MegaCanvas feature appears to be planned but not yet implemented in this branch.

### Issue #2: VideoHub Routing Alignment
User requested VideoHub routing to be aligned (input 1→output 1, input 2→output 2, input 6→output 6, etc.) for testing purposes.

## Solutions Implemented

### 1. VideoHub Routing Alignment (✅ COMPLETED)

Added new functionality to `VideoHubClient` class:

**New Method:** `AlignInputsToOutputs(const std::vector<int>& indices)`

This method allows routing multiple inputs to their corresponding outputs in a single command, useful for testing scenarios.

**Usage Example:**
```cpp
std::vector<int> testIndices = {0, 1, 5}; // 0-based indexing: inputs 1, 2, 6
videoHub.AlignInputsToOutputs(testIndices);
// Results in: Input 1→Output 1, Input 2→Output 2, Input 6→Output 6
```

**Integration:** The alignment test is now automatically run after Phase 1 during system initialization.

### 2. MegaCanvas Implementation (❌ NOT IMPLEMENTED)

The MegaCanvas feature requires significant implementation work:

**Required Components:**
1. `DXGIPresenter.cpp/.h` - DirectX presentation surface for WGC
2. `MegaCanvasManager.cpp/.h` - 16K atlas compositor
3. `AtlasCompositor.cu` - CUDA kernel for 4x3 grid composition
4. Integration with capture pipeline
5. CUDA/DX11 interop setup

**Why it's missing:**
- The current system uses Spout for vMix integration
- MegaCanvas would replace Spout with a native WGC window approach
- This is a major architectural change requiring:
  - New window creation (hidden at virtual coordinates)
  - DXGI swap chain setup
  - 16K atlas buffer management
  - CUDA-to-DX11 texture transfers

## Current vMix Integration

The system currently outputs to vMix via **Spout**:

1. DeckLink cards capture video → CUDA buffers
2. `SpoutManager` creates shared D3D11 textures
3. vMix receives via Spout input (Add Input → NDI/Desktop Capture → Spout tab)

**To use in vMix currently:**
- Add Input → More → Spout
- Select sender names like "VIB_CAM_01", "VIB_CAM_02", etc.

## Recommendations

### Short Term (Current PR)
✅ Use the VideoHub alignment function for testing
✅ Continue using Spout for vMix integration
⚠️ Update logs/documentation to remove MegaCanvas references until implemented

### Long Term (Future Work)
If MegaCanvas/WGC is desired over Spout:
1. Implement `DXGIPresenter` class for window creation
2. Create 16K atlas compositor
3. Set up CUDA/DX11 interop
4. Ensure window is visible to WGC but hidden from user
5. Test with vMix's Windows Graphics Capture

**Benefits of MegaCanvas approach:**
- Single unified 16K window with all 12 cameras
- Potentially lower latency than Spout
- Native WGC integration

**Benefits of current Spout approach:**
- Already implemented and working
- Individual camera control in vMix
- Well-tested SDK
- Lower complexity

## Testing the Current Implementation

1. Build the project in Visual Studio 2022
2. Run `VIB.exe`
3. Check logs for:
   ```
   [INFO] === VideoHub Routing Alignment Test ===
   [INFO] VideoHub alignment test completed successfully
   [INFO] Indices aligned:
   [INFO]   Input 1 -> Output 1
   [INFO]   Input 2 -> Output 2
   [INFO]   Input 6 -> Output 6
   ```
4. For vMix: Add Spout inputs (not WGC) to receive video

## Files Modified in This PR

- `src/control/VideoHubClient.h` - Added `AlignInputsToOutputs()` declaration
- `src/control/VideoHubClient.cpp` - Implemented alignment function
- `src/core/main.cpp` - Added test call during initialization
- `MEGACANVAS_ISSUE.md` - This documentation

## Conclusion

The "MegaCanvas window not appearing in vMix" issue is expected because:
1. MegaCanvas is not implemented in the current codebase
2. The system uses Spout, which works differently (sender/receiver model)

The VideoHub routing alignment has been implemented as requested for testing purposes.
