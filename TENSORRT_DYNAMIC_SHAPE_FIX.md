# TensorRT Dynamic Shape Inference Fix

## Problem
The application was experiencing TensorRT inference errors when running with a dynamic batch size engine:

```
TensorRT: IExecutionContext::enqueueV3: Error Code 3: API Usage Error 
(Parameter check failed, condition: inputDimensionSpecified && inputShapesSpecified. 
Not all shapes are specified. Following input tensors' dimensions are not specified: images.
```

## Root Cause
The TensorRT engine was built with dynamic shapes (indicated by `-1` in the batch dimension: `-1x3x1088x1920`). When an engine has dynamic dimensions, the actual input shape must be explicitly set before calling `enqueueV3()` using the `setInputShape()` API.

The existing code only called:
- `setTensorAddress()` - to bind GPU buffers
- `enqueueV3()` - to execute inference

But it was missing the required `setInputShape()` call for dynamic shape engines.

## Solution
Added support for dynamic shape engines in the InferenceEngine class:

### 1. Detection of Dynamic Shapes (LoadEngine)
Added logic to detect dynamic shapes when loading the engine:
```cpp
// Check for dynamic shapes (indicated by -1 in dimensions)
m_hasDynamicShapes = false;
for (int d = 0; d < dims.nbDims; ++d) {
    if (dims.d[d] == -1) {
        m_hasDynamicShapes = true;
        Logger::Info("InferenceEngine: Detected dynamic shape in dimension " + std::to_string(d));
        break;
    }
}
```

### 2. Set Input Shape Before Inference
Added `setInputShape()` calls in both inference paths:

**ProcessFrameUYVY (single frame):**
```cpp
if (m_hasDynamicShapes) {
    nvinfer1::Dims inputDims;
    inputDims.nbDims = 4;
    inputDims.d[0] = 1;  // Batch size = 1 for single frame
    inputDims.d[1] = kInputChannels;  // RGB channels
    inputDims.d[2] = m_config.inputHeight;
    inputDims.d[3] = m_config.inputWidth;
    
    if (!m_context->setInputShape(m_inputTensorName.c_str(), inputDims)) {
        Logger::Error("InferenceEngine: Failed to set input shape for dynamic engine");
        return {};
    }
}
```

**ProcessBatch (multiple frames):**
```cpp
if (m_hasDynamicShapes) {
    nvinfer1::Dims inputDims;
    inputDims.nbDims = 4;
    inputDims.d[0] = numFrames;  // Actual batch size
    inputDims.d[1] = kInputChannels;  // RGB channels
    inputDims.d[2] = m_config.inputHeight;
    inputDims.d[3] = m_config.inputWidth;
    
    if (!m_context->setInputShape(m_inputTensorName.c_str(), inputDims)) {
        Logger::Error("InferenceEngine: Failed to set input shape for dynamic engine");
        return {};
    }
}
```

### 3. Thread Safety Improvement
Also added mutex protection to `ProcessFrameUYVY` to ensure thread-safe access to the IExecutionContext. Previously, this function was missing mutex protection while calling `setTensorAddress()` and `enqueueV3()`, which could cause race conditions when multiple capture threads call inference simultaneously.

## Files Modified
- `src/ai/InferenceEngine.h` - Added `m_hasDynamicShapes` member variable
- `src/ai/InferenceEngine.cpp` - Added dynamic shape detection and setInputShape calls

## Testing
After applying this fix, the application should:
1. Detect dynamic shapes during engine loading and log the detection
2. Set input shapes appropriately before each inference call
3. Successfully execute inference without the "dimensions are not specified" error

## Expected Log Output
With the fix, you should see:
```
[INFO] InferenceEngine: Input tensor 'images' shape: -1x3x1088x1920
[INFO] InferenceEngine: Detected dynamic shape in dimension 0
```

And no more `enqueueV3` errors.

## Backwards Compatibility
The fix is backwards compatible:
- For engines without dynamic shapes, `m_hasDynamicShapes` will be `false`, and the `setInputShape()` calls are skipped
- For engines with dynamic shapes, the shapes are now properly set before inference
