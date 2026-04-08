/**
 * DXGIPresenter.h
 * 
 * Low-latency DXGI presentation for Windows Graphics Capture (WGC)
 * Implements FLIP_DISCARD swap chain with Frame Latency Waitable Object
 * 
 * Key optimizations:
 * - DXGI_SWAP_EFFECT_FLIP_DISCARD: Bypasses Desktop Window Manager (DWM)
 * - Frame Latency Waitable Object: Precise VBlank synchronization
 * - AllowTearing: VRR support for minimum latency
 * - WS_EX_NOREDIRECTIONBITMAP: Optimized for WGC capture
 * 
 * Window Configuration:
 * - WS_POPUP (borderless, no decorations)
 * - Off-screen positioning allowed (vMix reads GPU texture directly)
 * - No physical 16K monitor required
 */

#pragma once

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <Windows.h>
#include <string>
#include <atomic>

#pragma comment(lib, "dwmapi.lib")

using Microsoft::WRL::ComPtr;

/**
 * DXGI Presenter for 16K Mega-Canvas
 * 
 * Provides direct GPU presentation bypassing the Windows compositor
 * Optimized for vMix Windows Graphics Capture integration
 */
class DXGIPresenter {
public:
    /**
     * Configuration for the presenter
     */
    struct Config {
        int width = 15360;              // Canvas width (16K)
        int height = 6480;              // Canvas height
        int bufferCount = 2;            // Double-buffer for FLIP model
        bool enableTearing = true;      // VRR/FreeSync support
        bool enableWaitableObject = true;// Frame latency control
        bool borderless = true;         // WS_POPUP window
        bool noRedirectionBitmap = true;// WS_EX_NOREDIRECTIONBITMAP
        bool topmost = false;           // HWND_TOPMOST
        int windowX = 0;                // Window X position
        int windowY = 0;                // Window Y position
        std::wstring windowTitle = L"VIB MegaCanvas 16K"; // Window title
        
        // Headless mode options (Ghost Window)
        bool hiddenMode = false;        // Enable headless/ghost window mode
        bool mousePassthrough = false;  // WS_EX_TRANSPARENT + WS_EX_LAYERED for click-through
        bool hideFromTaskbar = false;   // WS_EX_TOOLWINDOW to hide from taskbar/Alt+Tab
        bool excludeFromCapture = false;// SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
                                        // WARNING: May affect some capture methods - test carefully
        int virtualX = -15360;          // DEPRECATED: Window now auto-positioned at screenWidth-1 for vMix WGC
        int virtualY = 0;               // DEPRECATED: Window now auto-positioned at screenHeight-1 for vMix WGC
                                        // vMix WGC requires >=1 pixel visible on primary monitor to detect window
    };

    DXGIPresenter();
    ~DXGIPresenter();

    // Disable copy/move
    DXGIPresenter(const DXGIPresenter&) = delete;
    DXGIPresenter& operator=(const DXGIPresenter&) = delete;
    DXGIPresenter(DXGIPresenter&&) = delete;
    DXGIPresenter& operator=(DXGIPresenter&&) = delete;

    /**
     * Initialize the DXGI presenter
     * Creates D3D11 device, swap chain, and borderless window
     * @param config Configuration settings
     * @return true if initialization successful
     */
    bool Initialize(const Config& config);

    /**
     * Shutdown and release all resources
     */
    void Shutdown();

    /**
     * Get the D3D11 device
     * Used for CUDA/DX11 interop registration
     */
    ID3D11Device* GetDevice() const { return m_device.Get(); }

    /**
     * Get the D3D11 device context
     */
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }

    /**
     * Get the swap chain
     */
    IDXGISwapChain1* GetSwapChain() const { return m_swapChain.Get(); }

    /**
     * Get the backbuffer texture
     * Used for CUDA/DX11 interop
     */
    ID3D11Texture2D* GetBackBuffer() const { return m_backBuffer.Get(); }

    /**
     * Get the window handle
     * Used for WGC target identification
     */
    HWND GetWindowHandle() const { return m_window; }

    /**
     * Wait for the frame latency waitable object
     * Call before rendering to ensure previous frame is presented
     * @return true if wait succeeded
     */
    bool WaitForVBlank();

    /**
     * Present the current backbuffer
     * Uses FLIP_DISCARD with optional tearing
     * @return true if present succeeded
     */
    bool Present();

    /**
     * Resize the swap chain (if window size changes)
     * @param width New width
     * @param height New height
     * @return true if resize succeeded
     */
    bool Resize(int width, int height);

    /**
     * Check if presenter is initialized
     */
    bool IsInitialized() const { return m_initialized.load(std::memory_order_acquire); }

    /**
     * Check if tearing is supported and enabled
     */
    bool IsTearingEnabled() const { return m_tearingSupported && m_config.enableTearing; }

    /**
     * Get presentation statistics
     */
    float GetFrameLatencyMs() const { return m_lastFrameLatencyMs.load(std::memory_order_relaxed); }
    int GetPresentCount() const { return m_presentCount.load(std::memory_order_relaxed); }
    int GetDroppedFrameCount() const { return m_droppedFrames.load(std::memory_order_relaxed); }

    /**
     * Get configuration
     */
    const Config& GetConfig() const { return m_config; }

private:
    // Internal initialization
    bool CreateDevice();
    bool CreateSwapChain();
    bool CreateRenderTarget();
    HWND CreateBorderlessWindow();
    bool CheckTearingSupport();
    
    // Window message handling (static callback)
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // D3D11 resources
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain1> m_swapChain;
    ComPtr<ID3D11Texture2D> m_backBuffer;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    
    // DXGI factory and adapter
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<IDXGIAdapter1> m_adapter;
    
    // Frame latency waitable object
    HANDLE m_frameLatencyWaitable = nullptr;
    
    // Window
    HWND m_window = nullptr;
    HINSTANCE m_hInstance = nullptr;
    std::wstring m_windowClassName;
    
    // Configuration
    Config m_config;
    bool m_tearingSupported = false;
    
    // State
    std::atomic<bool> m_initialized{false};
    
    // Statistics
    std::atomic<float> m_lastFrameLatencyMs{0.0f};
    std::atomic<int> m_presentCount{0};
    std::atomic<int> m_droppedFrames{0};
    
    // Timing for latency measurement
    LARGE_INTEGER m_perfFrequency;
    LARGE_INTEGER m_lastPresentTime;
};
