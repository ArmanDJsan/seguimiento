# Frame Saving Feature

## Overview

This feature automatically saves detection frames to verify camera input is being received correctly. Frames are saved in PPM (Portable PixMap) format, which requires no external dependencies.

## How It Works

### Automatic Frame Capture

The system automatically captures and saves frames when detections occur:

1. **Running Mode**: Saves one frame per camera when first detection occurs
   - Files: `detection_cam01.ppm`, `detection_cam02.ppm`, etc.
   - Location: Working directory where the application runs

2. **Radar Test Mode**: Saves one frame when first detection occurs
   - File: `detection_frame.ppm`
   - Location: Working directory where the application runs

### Frame Format

- **Format**: PPM (Portable PixMap) - P6 binary RGB format
- **No Dependencies**: Can be viewed with most image viewers without installing additional codecs
- **Source**: Converted from UYVY (camera native format) to RGB
- **Resolution**: Native camera resolution (typically 3840x2160 for 4K)

## Implementation Details

### FrameSaver Utility (`src/utils/FrameSaver.h`)

Provides three methods for saving frames:

1. **SaveBGRAFrame**: Saves BGRA buffer from GPU to PPM
   ```cpp
   FrameSaver::SaveBGRAFrame(cudaBuffer, width, height, "output.ppm");
   ```

2. **SaveYUVFrame**: Saves raw YUV/UYVY data
   ```cpp
   FrameSaver::SaveYUVFrame(cudaBuffer, width, height, "output.yuv");
   ```

3. **SaveYUVFrameAsPPM**: Converts UYVY to RGB and saves as PPM (recommended)
   ```cpp
   FrameSaver::SaveYUVFrameAsPPM(cudaBuffer, width, height, "output.ppm");
   ```

### Color Conversion

YUV to RGB conversion uses ITU-R BT.601 standard:
- Input: UYVY format (2 bytes per pixel, packed)
- Output: RGB format (3 bytes per pixel)
- Process: Handled automatically by FrameSaver utility

### Performance Impact

- **Minimal**: Frame saving only happens once per camera
- **Async**: CUDA memory copy is performed asynchronously
- **No Latency**: Does not block real-time video processing

## Viewing Saved Frames

PPM files can be opened with:

### Windows
- Windows Photo Viewer (some versions)
- IrfanView (free)
- GIMP (free)
- Paint.NET (free)
- XnView (free)

### Linux
- GIMP
- ImageMagick: `display detection_frame.ppm`
- Convert to PNG: `convert detection_frame.ppm detection_frame.png`

### macOS
- Preview (may need to convert first)
- GIMP
- Convert to PNG: `sips -s format png detection_frame.ppm --out detection_frame.png`

## Converting PPM to Other Formats

### Using ImageMagick (recommended)
```bash
# Convert to PNG
convert detection_frame.ppm detection_frame.png

# Convert to JPEG
convert detection_frame.ppm detection_frame.jpg

# Convert all PPM files in directory
mogrify -format png *.ppm
```

### Using Python (PIL/Pillow)
```python
from PIL import Image
img = Image.open('detection_frame.ppm')
img.save('detection_frame.png')
```

## Troubleshooting

### Frame Not Saved

If no frame is saved, check:
1. Are detections occurring? (Check logs for "DETECCION" messages)
2. Is the camera receiving video signal?
3. Are file write permissions correct in the working directory?
4. Is there enough disk space?

### Frame Appears Black/Corrupted

Possible causes:
1. Camera not sending valid signal
2. Wrong resolution settings
3. GPU buffer not properly allocated
4. Color conversion issue

Check the logs for any CUDA errors or camera initialization warnings.

### File Size

Expected file sizes for different resolutions:
- 4K (3840x2160): ~24 MB
- 1080p (1920x1080): ~6 MB
- 720p (1280x720): ~2.7 MB

If file size is significantly different, the frame may be corrupted or resolution may be incorrect.

## Log Messages

When frames are saved, you'll see messages like:
```
[INFO] Guardando frame de detección para verificación...
[INFO] Frame guardado exitosamente en detection_frame.ppm
[INFO] Dimensiones: 3840x2160
```

Or in Running Mode:
```
[INFO] Frame guardado: detection_cam01.ppm (3840x2160) - 3 detecciones
```

## Technical Notes

### Why PPM Format?

1. **No Dependencies**: Standard format, no external libraries needed
2. **Simple**: Easy to implement and debug
3. **Universal**: Supported by virtually all image viewers
4. **Raw Data**: Preserves original image quality
5. **Text Header**: Easy to verify file integrity

### Memory Management

- Uses CUDA's `cudaMemcpy` for device-to-host transfer
- Memory is allocated only during save operation
- Automatic cleanup via std::vector RAII

### Thread Safety

- Each camera uses its own atomic flag
- Frame saving is protected by the detection processing flow
- No race conditions possible

## Future Enhancements

Potential improvements:
1. Optional JPEG compression (requires external library)
2. Configurable save location
3. Timestamp in filename
4. Detection overlay (bounding boxes)
5. Multiple frame capture mode
