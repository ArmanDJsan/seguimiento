# VIB System Optimization Guide
## Threadripper PRO 9955WX + RTX 5080 Configuration

This guide ensures maximum performance for the VIB (Visual Intelligence Bypass) system under high-motion scenarios.

## Table of Contents
1. [Hardware Accelerated GPU Scheduling](#hardware-accelerated-gpu-scheduling)
2. [PCIe Configuration Verification](#pcie-configuration-verification)
3. [Windows Power Settings](#windows-power-settings)
4. [BIOS Configuration](#bios-configuration)
5. [Driver Updates](#driver-updates)
6. [Performance Validation](#performance-validation)

---

## 1. Hardware Accelerated GPU Scheduling

**Required**: This setting MUST be enabled for optimal GPU-CPU coordination.

### Why It Matters
- Reduces CPU intervention in GPU scheduling by 40-60%
- Allows GPU to manage its own command queues (VRAM-based scheduling)
- Critical for maintaining <32ms frame times under motion

### How to Enable (Windows 11)

**Method 1: Settings UI (Recommended)**
1. Open **Settings** (Win + I)
2. Navigate to **System** → **Display**
3. Scroll down and click **Graphics**
4. Click **Change default graphics settings**
5. Toggle **Hardware-accelerated GPU scheduling** to **ON**
6. **Restart the computer** (required for changes to take effect)

**Method 2: Registry (Advanced)**
1. Open **Registry Editor** as Administrator
2. Navigate to:
   ```
   HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\GraphicsDrivers
   ```
3. Create or modify **DWORD** value: `HwSchMode`
4. Set value to **2** (1 = Disabled, 2 = Enabled)
5. **Restart the computer**

### Verification
Run VIB - the startup log will show:
```
Hardware Accelerated GPU Scheduling: ENABLED
```

If you see `DISABLED`, follow the steps above.

---

## 2. PCIe Configuration Verification

**Required**: RTX 5080 must be connected to a PCIe 5.0 x16 slot.

### Expected Configuration
- **Link Width**: x16 (16 lanes)
- **Link Generation**: Gen 5 (or Gen 4 minimum)
- **Bandwidth**: 63 GB/s bidirectional (Gen5 x16) or 32 GB/s (Gen4 x16)

### How to Verify

**Method 1: VIB Startup Logs**
Look for this line in the startup sequence:
```
PCIe Link: Gen5 x16 (63.0 GB/s bidirectional) - OPTIMAL
```

**Warning indicators:**
- `Gen4 x8` → Only 16 GB/s → Move GPU to primary slot
- `Gen3 x16` → Only 15.8 GB/s → Enable Gen4/Gen5 in BIOS

**Method 2: GPU-Z**
1. Download GPU-Z: https://www.techpowerup.com/gpuz/
2. Launch GPU-Z
3. Check **Bus Interface**: Should show `PCIe x16 5.0 @ x16 5.0`
4. If it shows `@ x8` or `@ x4`, the card is not in optimal slot

### Troubleshooting

**Problem**: GPU shows x8 or x4 instead of x16
- **Solution**: Install GPU in the primary PCIe slot (usually top slot near CPU)
- **Check**: Other PCIe devices may be sharing lanes (disable unused slots in BIOS)

**Problem**: GPU shows Gen3 or Gen4 instead of Gen5
- **Solution**: Update motherboard BIOS to latest version
- **Check**: Enable "PCIe Gen 5" or "PCIe Speed: Auto" in BIOS

---

## 3. Windows Power Settings

**Required**: Disable power-saving features that throttle performance.

### Windows Power Plan
1. Open **Power Options** (Win + R → `powercfg.cpl`)
2. Select **High performance** or **Ultimate Performance**
   - If "Ultimate Performance" is not visible, run as Administrator:
     ```powershell
     powercfg -duplicatescheme e9a42b02-d5df-448d-aa00-03f14749eb61
     ```
3. Click **Change plan settings** → **Change advanced power settings**
4. Configure:
   - **Processor power management** → **Minimum processor state**: **100%**
   - **PCI Express** → **Link State Power Management**: **Off**
   - **USB settings** → **USB selective suspend**: **Disabled**

### Graphics Power Settings
1. Right-click Desktop → **Display settings**
2. **Graphics** → **Graphics performance preference**
3. Click **Browse** and add `VIB.exe`
4. Select **VIB.exe** → **Options** → **High performance**

### NVIDIA Control Panel
1. Open **NVIDIA Control Panel**
2. **Manage 3D settings** → **Global Settings**
3. Set:
   - **Power management mode**: **Prefer maximum performance**
   - **Low Latency Mode**: **Ultra** (reduces input lag)

---

## 4. BIOS Configuration

**Critical**: Threadripper PRO specific optimizations.

### Access BIOS
1. Restart computer
2. Press **Del** or **F2** during boot (depends on motherboard)

### Required Settings

#### CPU Configuration
- **AMD CBS** → **CPU Common Options**
  - **Global C-state Control**: **Disabled** (prevents CPU sleep)
  - **Core Performance Boost**: **Enabled** (allows 5.7 GHz boost)
  - **SMT Control**: **Enabled** (Simultaneous Multithreading - essential)

#### Memory Configuration
- **AMD Overclocking** → **Memory Overclocking**
  - Enable **XMP** or **EXPO** profile if using DDR5-5200+ RAM
  - Verify memory runs at rated speed (check with CPU-Z)

#### PCIe Configuration
- **Advanced** → **PCI Subsystem Settings**
  - **PCIe Speed**: **Gen 5** or **Auto**
  - **Above 4G Decoding**: **Enabled** (required for GPUs >4GB VRAM)
  - **Resizable BAR**: **Enabled** (allows CPU direct access to full VRAM)

#### Chipset Configuration
- **AMD CBS** → **NBIO Common Options**
  - **PCIe ARI Support**: **Enabled**
  - **PCIe ACS Enable**: **Auto** or **Disabled** (improves PCIe throughput)

---

## 5. Driver Updates

**Recommended**: Keep drivers up-to-date for bug fixes and performance improvements.

### NVIDIA GPU Driver
- Minimum version: **566.03** (first RTX 50-series support)
- Recommended: **Latest Studio Driver** from https://www.nvidia.com/drivers
- Install **Clean Installation** option to remove old driver remnants

### AMD Chipset Driver
- Download from: https://www.amd.com/en/support
- Select: **Ryzen Threadripper PRO**
- Install **AMD Chipset Drivers** (includes USB, SATA, PCIe optimizations)

### Blackmagic DeckLink Driver
- Version: **12.9** or later
- Download from: https://www.blackmagicdesign.com/support
- Verify 4-channel mode with **Desktop Video Setup** utility

### NDI SDK
- Version: **6.0** or later
- Installed at: `C:\Program Files\NDI\NDI 6 SDK`
- Required libraries: `Processing.NDI.Lib.x64.dll`

---

## 6. Performance Validation

Run VIB and verify the system meets performance targets.

### Success Criteria (High Motion Scenarios)

When **Avg motion > 0.5**, log output should show:

```
[PERF] Cap:8.2ms Sel:2.1ms YOLO:12.3ms NDI:0.9ms Redis:0.2ms Total:23.7ms Active:4/12
```

**Target KPIs:**
- **Cap** (Capture): < 15ms ✓
- **Sel** (Selection): < 5ms ✓
- **NDI** (Network): < 10ms ✓
- **Total**: < 32ms ✓

### Warning Signs

**Bad Example** (needs optimization):
```
[PERF] Cap:45.2ms Sel:18.3ms YOLO:12.1ms NDI:0.5ms Redis:0.1ms Total:76.2ms Active:4/12
```

**Problems:**
- Cap > 15ms → DMA transfer slow (check PCIe bandwidth)
- Sel > 5ms → Motion detection bottleneck (check GPU utilization)
- NDI < 1ms with high Total → Frame dropping (starvation)

### Monitoring Tools

**Real-Time Dashboard**
```bash
monitor.bat
```

**Expected Output:**
```
VIB System Monitor
==================
GPU: RTX 5080
VRAM: 1650 MB / 16384 MB (10%)
Temp: 62°C
Util: 45%

Frame Timing (ms)
  Cap: 8.2  Sel: 2.1  YOLO: 12.3  NDI: 0.9
  Total: 23.7 (Target: <32.0)

Active Cameras: 4/12
Motion Level: HIGH (0.67)
```

---

## 7. Troubleshooting Common Issues

### Issue: "Hardware Accelerated GPU Scheduling: DISABLED"
**Solution**: Follow [Section 1](#hardware-accelerated-gpu-scheduling)

### Issue: "PCIe Link: Gen3 x8 - SUBOPTIMAL"
**Solution**: 
1. Move GPU to primary PCIe slot (nearest to CPU)
2. Update motherboard BIOS
3. Enable PCIe Gen5 in BIOS
4. Remove unused PCIe cards that share lanes

### Issue: High Cap times (>20ms)
**Cause**: DMA transfer bottleneck
**Solution**:
1. Verify PCIe x16 (not x8)
2. Disable PCIe power saving (Section 3)
3. Check DeckLink driver version (should be 12.9+)

### Issue: High Sel times (>8ms)
**Cause**: GPU not processing motion detection efficiently
**Solution**:
1. Verify HAGS is enabled
2. Check GPU utilization (should be 30-50%, not 95%+)
3. Reduce active cameras (F2 key for thermal throttle)

### Issue: NDI very low (<0.5ms) but Total very high (>40ms)
**Cause**: Frame starvation - NDI is dropping frames, not being efficient
**Solution**:
1. This is NOT a good sign - it means upstream pipeline is slow
2. Check Cap and Sel times (these are the real bottlenecks)
3. Apply optimizations above

---

## 8. Expected Performance Profile

### Baseline (No Motion)
- Cap: 0.5-2ms
- Sel: 0.3-0.5ms
- YOLO: 10-14ms (4 cameras, FP16)
- NDI: 0.8-1.0ms
- Total: 12-18ms

### High Motion (Racing Action)
- Cap: 5-12ms
- Sel: 1.5-4ms
- YOLO: 10-14ms (unchanged)
- NDI: 0.8-1.2ms
- Total: 18-31ms

### Alert Thresholds
- **Yellow Warning**: Total > 28ms (3 consecutive frames)
- **Red Alert**: Total > 33ms (frame budget exceeded)
- **Emergency**: Auto-reduce to 2 cameras if Total > 33ms for 10 frames

---

## 9. Contact and Support

If performance targets are not met after following this guide:

1. Collect logs from `logs/` directory
2. Run `monitor.bat` and take screenshot
3. Note hardware configuration (motherboard model, BIOS version)
4. Open GitHub issue with diagnostics

**Expected VIB startup should show:**
```
=== System Optimization Check ===
CPU: 32 logical processors, 16 physical cores (estimated)
Hardware Accelerated GPU Scheduling: ENABLED
PCIe Link: Gen5 x16 (63.0 GB/s bidirectional) - OPTIMAL
GPU: NVIDIA GeForce RTX 5080
  Compute Capability: 10.0
  Total VRAM: 16 GB
  CUDA Cores: 14080 (estimated)
  Memory Clock: 15000 MHz
  Memory Bus Width: 256-bit
  VRAM Free: 14 GB / 16 GB
System configuration is OPTIMAL for VIB
==================================
```

---

**Document Version**: 1.0  
**Last Updated**: 2026-04-06  
**Compatible with**: VIB v2.0+
