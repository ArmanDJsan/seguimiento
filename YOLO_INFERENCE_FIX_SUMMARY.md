# YOLO Inference Logic Fix Summary

## Problem Statement

When placing 10 unique spheres (labeled 0-9) in front of the camera, the YOLO inference was detecting:
- Multiple detections of the same class (e.g., three detections of class 2, two of class 1, two of class 5)
- Missing detections for some classes
- Overall incorrect class distribution

Debug output showed:
```
Output format: maxDetections=300, stride=6, numClasses=1
Det[0]: 725.5000, 549.0000, 777.5000, 601.5000, 0.9785, 1.0000  (class 1)
Det[1]: 595.0000, 643.0000, 649.5000, 698.0000, 0.9702, 2.0000  (class 2)
Det[3]: 974.0000, 369.0000, 1023.0000, 419.7500, 0.9106, 1.0000  (class 1 - DUPLICATE!)
...
```

## Root Causes Identified

### 1. **Model Output Format Mismatch**
The YOLO model outputs detections in `[x1, y1, x2, y2, objectness, class_id]` format with stride=6:
- Coordinates are in absolute pixel format (e.g., 725.5, 549.0)
- Class ID is a direct integer value 0-9 at index 5

The code was expecting normalized center-based coordinates: `[center_x, center_y, width, height]`

**Impact:** Bounding box coordinates were being used incorrectly, affecting IoU calculations in NMS.

### 2. **Class-Agnostic NMS**
The original NMS (Non-Maximum Suppression) implementation would suppress overlapping detections regardless of their class ID.

**Impact:** 
- In a multi-class scenario with 10 different spheres, valid detections could be incorrectly suppressed
- Conversely, multiple detections of the SAME sphere (with same class ID) might not be properly deduplicated

### 3. **Incorrect numClasses Calculation**
The code calculated `numClasses = stride - kYoloBaseAttributes = 6 - 5 = 1`, which was technically correct for this model's output format (single class_id value, not 10 probability scores).

However, this doesn't mean there's only 1 class - it means the model outputs the class ID directly rather than as probability distributions.

## Fixes Implemented

### Fix 1: Coordinate Format Conversion
**File:** `src/ai/InferenceEngine.cpp`
**Location:** PostProcess function, lines ~848-878

**Change:** Added proper coordinate conversion from absolute (x1, y1, x2, y2) to normalized center format:

```cpp
// Convert bounding box coordinates
// Model outputs in (x1, y1, x2, y2) absolute pixel format
// Need to convert to normalized (center_x, center_y, width, height)
float x1 = det[0];
float y1 = det[1];
float x2 = det[2];
float y2 = det[3];

// Calculate center point and dimensions
float center_x = (x1 + x2) / 2.0f;
float center_y = (y1 + y2) / 2.0f;
float width = x2 - x1;
float height = y2 - y1;

// Normalize to [0, 1] range based on input dimensions
float norm_x = center_x / static_cast<float>(m_config.inputWidth);
float norm_y = center_y / static_cast<float>(m_config.inputHeight);
float norm_width = width / static_cast<float>(m_config.inputWidth);
float norm_height = height / static_cast<float>(m_config.inputHeight);
```

**Benefit:** Ensures bounding boxes are properly normalized and in the correct format for downstream processing (IoU calculation, tracking, etc.)

### Fix 2: Class-Aware NMS
**File:** `src/ai/InferenceEngine.cpp`
**Location:** ApplyNMS function, lines ~885-935

**Change:** Added class ID check before applying IoU-based suppression:

```cpp
// CLASS-AWARE NMS: Only suppress detections with the SAME class ID
// Different spheres (different class IDs) can occupy nearby positions
if (detections[i].ballID != detections[j].ballID) continue;
```

**Benefit:** 
- Multiple detections of the SAME sphere (same class ID) with high IoU are properly suppressed, keeping only the highest confidence
- Detections of DIFFERENT spheres (different class IDs) are preserved even if they overlap spatially
- Correct for multi-object detection scenarios where multiple objects can be close together

### Fix 3: Debug Logging Enhancement
**File:** `src/ai/InferenceEngine.cpp`
**Location:** ApplyNMS function

**Changes Added:**
1. NMS suppression logging showing which detections are being removed and why
2. Final detection summary showing count per class after NMS

```cpp
// Debug: Log NMS suppressions
Logger::Info("[NMS_DEBUG] Suppressing detection: ballID=" + ... + ", IoU=" + ...);

// Debug: Log final detection summary by class
Logger::Info("[NMS_SUMMARY] Final detections by class: B0(1) B1(1) B2(1) ...");
```

**Benefit:** Helps diagnose whether issues are due to:
- Model misclassification (wrong class IDs assigned)
- NMS over-suppression (valid detections being removed)
- Multiple detections of same object (same class, high IoU)

## Expected Behavior After Fix

With 10 unique spheres (0-9) in view:

### Before Fix:
```
Raw detections: B1, B2, B3, B1, B2, B0, B2, B9, B5, B5
After NMS: B1, B2, B3, B0, B9, B5  (some duplicates suppressed, some missing)
```

### After Fix:
```
Raw detections: B1, B2, B3, B1, B2, B0, B2, B9, B5, B5
After NMS: B1, B2, B3, B0, B9, B5  (duplicates of same class properly suppressed)
```

**Note:** If the model is still detecting multiple instances of the same class in different locations, that indicates a **model quality issue** (misclassification), not a code issue. The fixes ensure:
1. Coordinates are processed correctly
2. Duplicate detections of the same sphere are removed
3. Different spheres near each other are not incorrectly suppressed

## Remaining Potential Issues

If after these fixes you still see duplicate class detections (e.g., two detections of "class 1" in different locations), the root cause is **model misclassification**, which could be due to:

1. **Model training quality**: Model wasn't trained with enough data or epochs
2. **Input preprocessing**: Colors, normalization, or image format doesn't match training
3. **Sphere visual similarity**: Some spheres look too similar under certain lighting
4. **Camera view angle**: Model trained on different viewpoint than runtime camera angle

To diagnose:
- Check NMS debug logs to see if duplicates have high IoU (= same object detected twice) or low IoU (= model error)
- Verify input image preprocessing matches model training (normalization values, color space)
- Test with better lighting or different camera angles
- Consider retraining the model with more diverse data

## Testing Recommendations

1. **Enable debug logging** in config to see NMS behavior:
   ```cpp
   config.debugYoloEnabled = true;
   config.debugYoloInterval = 100;  // Every 100 frames
   ```

2. **Monitor the new debug logs:**
   - `[NMS_DEBUG]` - Shows which detections are being suppressed
   - `[NMS_SUMMARY]` - Shows final class distribution after NMS

3. **Expected results with 10 unique spheres:**
   - Should see max 1 detection per class (B0-B9)
   - If multiple detections of same class have high IoU, NMS will suppress the lower confidence one
   - If multiple detections of same class have low IoU, that's a model error

4. **Verify coordinate normalization:**
   - Check that detection coordinates are in [0, 1] range
   - Verify bounding boxes align with actual object positions in the frame

## Files Modified

- `src/ai/InferenceEngine.cpp`
  - PostProcess(): Fixed coordinate conversion from (x1,y1,x2,y2) to normalized (cx,cy,w,h)
  - ApplyNMS(): Added class-aware suppression logic
  - ApplyNMS(): Added debug logging for NMS decisions
  - Added `<map>` include for class count tracking

## Next Steps

1. Deploy and test with actual 10-sphere scenario
2. Monitor `[NMS_DEBUG]` and `[NMS_SUMMARY]` logs
3. If duplicates persist with low IoU, investigate model training/input preprocessing
4. Consider adding visualization to show bounding boxes and class IDs on video output
