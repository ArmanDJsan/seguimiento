# Spout Removal - Completed

Spout SDK has been completely removed from this branch and replaced with NDI SDK 6.

## Files Removed:
- src/spout/SpoutManager.cpp (142 lines)
- src/spout/SpoutManager.h (61 lines)

## Current Video Output:
All video is now sent via NDI SDK 6 through NDIManager.
- 12 NDI senders: VIB_CAM_01 through VIB_CAM_12
- UYVY format (native DeckLink output)
- Native vMix support - no plugins required
