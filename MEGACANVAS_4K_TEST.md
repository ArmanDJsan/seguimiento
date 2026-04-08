# MegaCanvas 4K Test Configuration

## Summary
The MegaCanvas has been temporarily reduced from 16K (15360x6480) to 4K (3840x2160) to troubleshoot vMix hanging issues.

## Changes Made

### Canvas Resolution
- **Previous**: 16K (15360 × 6480)
- **Current**: 4K (3840 × 2160)
- **Reduction**: ~12x less pixels (~100M → ~8M pixels)

### Grid Layout
- **Previous**: 4×3 grid (12 cameras)
- **Current**: 2×2 grid (4 cameras)

### Cell Resolution
- **Previous**: 4K per cell (3840 × 2160)
- **Current**: HD per cell (1920 × 1080)

### VRAM Usage
- **Previous**: ~420 MB (398 MB atlas + 20 MB TensorRT buffers)
- **Current**: ~40 MB (33 MB atlas + 6.5 MB TensorRT buffers)

## Files Modified

1. **src/config.json**
   - Updated grid_layout: columns=2, rows=2
   - Updated cell_resolution: 1920×1080
   - Updated canvas_resolution: 3840×2160
   - Updated virtual_coordinates.x: -3840

2. **src/canvas/MegaCanvasManager.h**
   - NUM_CAMERAS: 12 → 4
   - GRID_COLS: 4 → 2
   - GRID_ROWS: 3 → 2
   - CELL_WIDTH: 3840 → 1920
   - CELL_HEIGHT: 2160 → 1080
   - CANVAS_WIDTH: 15360 → 3840
   - CANVAS_HEIGHT: 6480 → 2160

3. **src/canvas/DXGIPresenter.h**
   - Default width: 15360 → 3840
   - Default height: 6480 → 2160
   - Window title: "VIB MegaCanvas 16K" → "VIB MegaCanvas 4K TEST"
   - virtualX: -15360 → -3840

4. **src/canvas/MegaCanvasManager.cpp**
   - Updated window title to "VIB MegaCanvas 4K TEST (vMix WGC)"
   - Updated log messages to indicate 4K test configuration

5. **src/core/main.cpp**
   - Updated initialization messages to show 4K test configuration
   - Updated vMix instructions to select "VIB MegaCanvas 4K TEST" window
   - Added warning about test configuration

6. **src/canvas/AtlasCompositor.h/cu**
   - Updated comments to reflect 4K resolution
   - Updated parameter documentation

7. **src/canvas/DXGIPresenter.cpp**
   - Updated comments to reflect 4K resolution

## Testing Instructions

### Build the Project
```bash
# Navigate to repository
cd /path/to/seguimiento

# Build (assuming you have build scripts)
# Follow your standard build process
```

### Configure vMix
1. Launch vMix
2. Add Input → More → Windows Desktop Capture
3. Select window: **"VIB MegaCanvas 4K TEST (vMix WGC)"**
4. Enable "High Input Performance Mode" in vMix settings

### Expected Behavior
- vMix should **NOT** hang with 4K canvas
- Window will show 4 cameras in a 2×2 grid
- Each cell is HD resolution (1920×1080)
- Total canvas is standard 4K (3840×2160)

### What to Monitor
1. **vMix Stability**: Does vMix hang or crash with 4K canvas?
2. **Video Quality**: Is the 4K/HD video quality acceptable?
3. **Performance**: Check CPU/GPU usage, frame drops
4. **System Logs**: Look for any errors or warnings

## Reverting to 16K (If Needed)

If you need to restore the original 16K configuration:

1. In **src/config.json**:
   - grid_layout: columns=4, rows=3
   - cell_resolution: width=3840, height=2160
   - canvas_resolution: width=15360, height=6480
   - virtual_coordinates.x: -15360

2. In **src/canvas/MegaCanvasManager.h**:
   - NUM_CAMERAS = 12
   - GRID_COLS = 4, GRID_ROWS = 3
   - CELL_WIDTH = 3840, CELL_HEIGHT = 2160
   - CANVAS_WIDTH = 15360, CANVAS_HEIGHT = 6480

3. Update window titles back to "VIB MegaCanvas 16K"

## Next Steps

Based on test results:

### If 4K Works Without Hanging
- The issue is related to the large canvas size
- Consider intermediate solutions:
  - 8K canvas (7680×4320) - 2×2 grid with 4K cells
  - Optimize vMix settings for large canvases
  - Check for vMix version updates or patches

### If 4K Still Hangs
- The issue is not resolution-related
- Investigate:
  - DXGI/WGC compatibility
  - vMix configuration settings
  - GPU driver issues
  - Windows Graphics Capture limitations

## Notes

- This is a **TEST CONFIGURATION** only
- All changes are marked with "TEST" comments in code
- VRAM usage is minimal (~40 MB vs ~420 MB)
- Only 4 cameras will be active in this configuration
- TensorRT inference still uses 640×640 downscaled buffers

## Contact
If you have questions or need to revert changes, refer to git history:
```bash
git log --oneline
git revert <commit-hash>  # To undo these changes
```
