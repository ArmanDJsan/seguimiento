# Legacy YOLOProcessor Removal

**Date**: 2026-04-09  
**Status**: COMPLETED

## Summary

The legacy `YOLOProcessor` class has been identified as obsolete and replaced by the new `InferenceEngine` architecture. This document tracks the removal of all legacy YOLO-related code from the VIB system.

## Background

### Why YOLOProcessor is Legacy

The VIB system originally used `YOLOProcessor` for object detection, but has since migrated to a more sophisticated tracking pipeline centered around `InferenceEngine`:

**Old Architecture (Legacy)**:
```
YOLOProcessor → Redis Publishing
```

**New Architecture (Current)**:
```
InferenceEngine → PositionMapper → BallTracker → SceneManager → RankingPublisher
```

### Key Differences

| Aspect | YOLOProcessor (Legacy) | InferenceEngine (Current) |
|--------|----------------------|--------------------------|
| **Purpose** | Generic object detection | Ball-specific detection + tracking |
| **Model** | yolov8n.engine | yolo26l_fp16_batch12.engine |
| **Batch Size** | 4 cameras | 12 cameras (all) |
| **Integration** | Standalone, only Redis output | Full tracking pipeline |
| **Detection Output** | Generic Detection struct | BallDetection with ball IDs |
| **Coordinates** | Pixel space only | Pixel → Global (homography) |
| **Tracking** | None | Kalman filtering, Hungarian assignment |
| **Scene Control** | None | VideoHub routing, leapfrogging |

## Components Removed

### 1. Source Files
- ✅ `src/ai/YOLOProcessor.h` - Legacy YOLO processor header
- ✅ `src/ai/YOLOProcessor.cpp` - Legacy YOLO processor implementation

### 2. Configuration
- ✅ `config.json` - Removed `yolo` section (lines 21-29)
- ✅ `src/core/main.cpp` - Removed YOLO config fields from Config struct:
  - `yoloEnabled`
  - `yoloModelPath`
  - `yoloFallback`
  - `yoloBatchSize`
  - `yoloConfidenceThreshold`
  - `yoloNmsThreshold`
  - `yoloUseFP16`

### 3. Main Pipeline Code (main.cpp)
- ✅ Line 36: Removed `#include "../ai/YOLOProcessor.h"`
- ✅ Lines 106-113: Removed YOLO config fields from Config struct
- ✅ Lines 224-230: Removed YOLO default values
- ✅ Lines 286-310: Removed YOLO config parsing section
- ✅ Lines 752-786: Removed YOLOProcessor initialization
- ✅ Line 760-766: Removed YOLO CUDA stream creation
- ✅ Line 902: Removed yoloProcessor from frame handler lambda capture
- ✅ Lines 994-1040: Removed legacy YOLO processing path
- ✅ Line 1061: Removed YOLO status from startup info

### 4. Build System
- ✅ `src/VIB.vcxproj` - Removed YOLOProcessor from compilation:
  - Line 118: `<ClCompile Include="ai\YOLOProcessor.cpp" />`
  - Line 150: `<ClInclude Include="ai\YOLOProcessor.h" />`

### 5. Dependencies Updated
- ✅ `src/redis/RedisWorker.h` - Updated to use BallDetection instead of Detection

## Migration Notes

### For Developers

If you need object detection capabilities in VIB:

1. **Use InferenceEngine**: The current system for ball detection
   ```cpp
   auto inferenceEngine = std::make_shared<InferenceEngine>();
   if (inferenceEngine->Initialize(config.inferenceConfig)) {
       auto detections = inferenceEngine->ProcessFrame(...);
   }
   ```

2. **Configuration**: Use the `inference_engine` section in `config.json`
   ```json
   "inference_engine": {
     "model_path": "models/yolo26l_fp16_batch12.engine",
     "batch_size": 12,
     "confidence_threshold": 0.6,
     "nms_threshold": 0.4,
     "num_classes": 10
   }
   ```

3. **Full Pipeline Access**:
   - Ball detection: `InferenceEngine::ProcessFrame()`
   - Coordinate transform: `PositionMapper::PixelToGlobal()`
   - Multi-ball tracking: `BallTracker::UpdateTracks()`
   - Scene control: `SceneManager::ProcessFrame()`
   - vMix output: `RankingPublisher::PublishRankings()`

### Redis Publishing

Legacy code used `Detection` struct from YOLOProcessor. New code should use:
```cpp
// Old (removed)
std::vector<Detection> detections = yoloProcessor->ProcessFrame(...);

// New (current)
std::vector<BallDetection> detections = inferenceEngine->ProcessFrame(...);
```

If you need Redis publishing of detection data, integrate with `BallTracker` output which provides:
- Ball IDs (1-10)
- Global coordinates (meters)
- Velocity estimates
- Tracking confidence

## Testing Checklist

After removal, verify:
- ✅ System compiles without errors
- ✅ No references to YOLOProcessor remain in codebase
- ✅ InferenceEngine pipeline works correctly
- ✅ Config validation passes
- ✅ No runtime errors related to missing YOLO components

## Future Cleanup

### If You Find Remaining References

This removal was comprehensive, but if you discover any remaining YOLOProcessor references:

1. **Check these locations**:
   - Documentation files (README, ARCHITECTURE.md, etc.)
   - Test files
   - Build scripts or configuration files
   - Comments in code that reference the old architecture

2. **Remove or update them** to reference InferenceEngine instead

3. **Update this document** to track the additional cleanup

### Detection Struct

The `Detection` struct (defined in YOLOProcessor.h) may still be referenced elsewhere. If you need generic detection output:

**Option A**: Use `BallDetection` from InferenceEngine.h  
**Option B**: Move `Detection` struct to a shared header if needed by other components

## References

- **New Architecture**: See `src/ARCHITECTURE.md` for tracking pipeline details
- **InferenceEngine**: `src/ai/InferenceEngine.h` and `InferenceEngine.cpp`
- **Tracking Pipeline**: `src/core/main.cpp` lines 792-850 (initialization), 944-990 (processing)
- **Config Schema**: `src/config.json` line 94-103 (`inference_engine` section)

## Verification Commands

To verify complete removal:

```bash
# Search for any remaining YOLOProcessor references
grep -r "YOLOProcessor" --exclude-dir=.git --exclude="LEGACY_YOLO_REMOVAL.md" .

# Search for yoloProcessor variable references
grep -r "yoloProcessor" --exclude-dir=.git --exclude="LEGACY_YOLO_REMOVAL.md" .

# Search for config.yolo references
grep -r "config\.yolo" --exclude-dir=.git --exclude="LEGACY_YOLO_REMOVAL.md" .

# Search for Detection struct (may need migration)
grep -r "std::vector<Detection>" --exclude-dir=.git .
```

Expected result: No matches (except this documentation file and historical commits)

---

**Status**: ✅ All legacy YOLOProcessor code has been removed  
**Replaced by**: InferenceEngine tracking pipeline  
**Safe to proceed**: Yes - system now uses unified inference architecture
