# Spout Removal Completed

This branch contains the complete removal of Spout SDK from the project.

## Changes:
- Spout SDK completely removed
- All video output now uses NDI SDK 6
- 12 NDI senders: VIB_CAM_01 to VIB_CAM_12
- UYVY format (native DeckLink output)
- Zero-copy async sending

## Verification:
- No SpoutManager.cpp or SpoutManager.h files
- No Spout references in VIB.vcxproj
- NDI properly configured in main.cpp
- YOLOProcessor uses ID3D11Texture2D only for internal processing

