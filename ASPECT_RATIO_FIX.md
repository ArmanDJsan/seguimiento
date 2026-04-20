# Aspect Ratio Fix: 1280x720 Support for Model Input

## Problem
The model was trained at **1280x720** (16:9 aspect ratio), but the system was transforming images to **1280x1280** (square), causing:
- Image distortion (stretching/compressing)
- Loss of detection quality
- Incorrect aspect ratio mismatch between training and inference

## Root Cause
The configuration only supported a single `input_size` parameter which was used for both width and height, forcing square inputs:
```json
"input_size": 1280  // Was being interpreted as 1280x1280
```

## Solution
### 1. Updated Configuration Format
The system now supports separate `input_width` and `input_height` parameters:

**New format (recommended):**
```json
"inference_engine": {
  "input_width": 1280,
  "input_height": 720,
  ...
}
```

**Legacy format (still supported):**
```json
"inference_engine": {
  "input_size": 640,  // Creates 640x640 square input
  ...
}
```

### 2. Changes Made

#### Files Modified:
1. **src/core/main.cpp**
   - Added support for `input_width` and `input_height` config parameters
   - Maintained backward compatibility with `input_size`

2. **src/config.json**
   - Changed from `"input_size": 640` to `"input_width": 1280, "input_height": 720`
   - Updated model path to `"models/best.engine"`

3. **src/ai/InferenceEngine.h**
   - Updated default values from 640x640 to 1280x720
   - Added comments explaining aspect ratio

4. **CUDA Kernels** (PreprocessKernel.cu, FusedPreprocessKernel.cu)
   - Updated documentation to reflect support for non-square dimensions
   - No code changes needed - kernels already supported separate width/height

5. **src/ai/InferenceEngine.cpp**
   - Updated comments to reflect configurable dimensions

6. **src/capture/DeckLinkCapture.cpp**
   - Updated comment about preprocessing dimensions

### 3. Benefits
- **Correct Aspect Ratio**: Maintains 16:9 (3840x2160 → 1280x720)
- **No Distortion**: Objects preserve their real-world proportions
- **Better Detection**: Model sees images in the same format it was trained on
- **Flexible**: Supports both square and rectangular inputs

### 4. Camera to Model Transformation
- **Source**: 3840x2160 (4K, 16:9 aspect ratio)
- **Destination**: 1280x720 (16:9 aspect ratio preserved)
- **Method**: Bilinear interpolation in CUDA kernel
- **Color Space**: UYVY → RGB conversion with BT.709

## Testing
1. Build the project in Visual Studio
2. Run the application with the updated config
3. Verify detection coordinates are accurate
4. Check that aspect ratio is preserved in output

## Expected Results
- Improved detection accuracy (objects are not distorted)
- Coordinates should now align correctly with the 16:9 aspect ratio
- No performance impact (CUDA kernels already supported this)

## Backward Compatibility
Projects using the old `input_size` parameter will continue to work with square inputs:
```json
"input_size": 640  // Still creates 640x640 (square)
```

To migrate to the new format, replace:
```json
"input_size": X
```

With:
```json
"input_width": X,
"input_height": Y
```

## Notes
- The CUDA kernels were already designed to support non-square dimensions
- Only configuration parsing and defaults needed updating
- The fused UYVY→RGB kernel efficiently handles any target resolution
- Memory allocation is automatically adjusted based on configured dimensions
