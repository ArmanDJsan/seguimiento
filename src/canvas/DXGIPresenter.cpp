/**
 * DXGIPresenter.cpp
 * 
 * Implementation of low-latency DXGI presentation
 * Optimized for 16K Mega-Canvas with WGC capture
 * 
 * Key Implementation Details:
 * - DXGI_SWAP_EFFECT_FLIP_DISCARD bypasses DWM for minimum latency
 * - Frame Latency Waitable Object synchronizes with display refresh
 * - AllowTearing flag enables VRR for sub-frame latency
 * - WS_EX_NOREDIRECTIONBITMAP optimizes for WGC capture
 */

#include "DXGIPresenter.h"
#include "../utils/Logger.h"

#include <string>
#include <sstream>
#include <iomanip>

// Link required libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {
    // Window class name for the borderless window
    constexpr wchar_t kWindowClassName[] = L"VIB_MegaCanvas_WGC";
    
    // Helper to format HRESULT as hexadecimal
    std::string HResultToHex(HRESULT hr) {
        std::ostringstream oss;
        oss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << static_cast<unsigned long>(hr);
        return oss.str();
    }
}

DXGIPresenter::DXGIPresenter() {
    QueryPerformanceFrequency(&m_perfFrequency);
    m_lastPresentTime.QuadPart = 0;
    m_hInstance = GetModuleHandle(nullptr);
}

DXGIPresenter::~DXGIPresenter() {
    Shutdown();
}

bool DXGIPresenter::Initialize(const Config& config) {
    if (m_initialized.load(std::memory_order_acquire)) {
        Logger::Warning("DXGIPresenter already initialized");
        return true;
    }

    m_config = config;
    
    Logger::Info("DXGIPresenter: Initializing " + 
                 std::to_string(config.width) + "x" + std::to_string(config.height) + 
                 " presentation surface...");

    // Create D3D11 device first
    if (!CreateDevice()) {
        Logger::Error("DXGIPresenter: Failed to create D3D11 device");
        return false;
    }

    // Check tearing support for VRR
    if (!CheckTearingSupport()) {
        Logger::Warning("DXGIPresenter: Tearing not supported - VSync will be enforced");
    }

    // Create borderless window for WGC capture
    m_window = CreateBorderlessWindow();
    if (!m_window) {
        Logger::Error("DXGIPresenter: Failed to create borderless window");
        Shutdown();
        return false;
    }

    // Create swap chain with FLIP_DISCARD
    if (!CreateSwapChain()) {
        Logger::Error("DXGIPresenter: Failed to create swap chain");
        Shutdown();
        return false;
    }

    // Create render target view
    if (!CreateRenderTarget()) {
        Logger::Error("DXGIPresenter: Failed to create render target");
        Shutdown();
        return false;
    }

    m_initialized.store(true, std::memory_order_release);
    
    Logger::Info("DXGIPresenter: Initialized successfully");
    Logger::Info("  Window: " + std::to_string(reinterpret_cast<uintptr_t>(m_window)));
    Logger::Info("  Tearing: " + std::string(m_tearingSupported ? "ENABLED" : "DISABLED"));
    Logger::Info("  Waitable Object: " + std::string(m_frameLatencyWaitable ? "ENABLED" : "DISABLED"));
    
    return true;
}

void DXGIPresenter::Shutdown() {
    m_initialized.store(false, std::memory_order_release);
    
    // Close waitable object handle
    if (m_frameLatencyWaitable) {
        CloseHandle(m_frameLatencyWaitable);
        m_frameLatencyWaitable = nullptr;
    }
    
    // Release render target
    m_rtv.Reset();
    m_backBuffer.Reset();
    
    // Release swap chain
    m_swapChain.Reset();
    
    // Release device
    m_context.Reset();
    m_device.Reset();
    
    // Release DXGI resources
    m_adapter.Reset();
    m_factory.Reset();
    
    // Destroy window
    if (m_window) {
        DestroyWindow(m_window);
        m_window = nullptr;
    }
    
    // Unregister window class
    if (!m_windowClassName.empty()) {
        UnregisterClassW(m_windowClassName.c_str(), m_hInstance);
        m_windowClassName.clear();
    }
    
    Logger::Info("DXGIPresenter: Shutdown complete");
}

bool DXGIPresenter::CreateDevice() {
    // Create DXGI Factory
    UINT factoryFlags = 0;
#ifdef _DEBUG
    factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

    HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory));
    if (FAILED(hr)) {
        Logger::Error("DXGIPresenter: CreateDXGIFactory2 failed: " + HResultToHex(hr));
        return false;
    }

    // Enumerate adapters and select high-performance GPU
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(m_factory.As(&factory6))) {
        hr = factory6->EnumAdapterByGpuPreference(
            0,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&adapter)
        );
    }
    
    if (!adapter) {
        // Fallback to first adapter
        hr = m_factory->EnumAdapters1(0, &adapter);
        if (FAILED(hr)) {
            Logger::Error("DXGIPresenter: No DXGI adapter found");
            return false;
        }
    }
    
    m_adapter = adapter;
    
    // Log adapter info
    DXGI_ADAPTER_DESC1 adapterDesc;
    m_adapter->GetDesc1(&adapterDesc);
    
    char adapterName[128];
    WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, adapterName, 128, nullptr, nullptr);
    Logger::Info("DXGIPresenter: Using adapter: " + std::string(adapterName));
    Logger::Info("  Dedicated VRAM: " + std::to_string(adapterDesc.DedicatedVideoMemory / (1024*1024)) + " MB");

    // Create D3D11 device
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    
    UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevel;
    hr = D3D11CreateDevice(
        m_adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,  // Must use UNKNOWN when specifying adapter
        nullptr,
        deviceFlags,
        featureLevels,
        _countof(featureLevels),
        D3D11_SDK_VERSION,
        &m_device,
        &featureLevel,
        &m_context
    );

    if (FAILED(hr)) {
        Logger::Error("DXGIPresenter: D3D11CreateDevice failed: " + HResultToHex(hr));
        return false;
    }

    Logger::Info("DXGIPresenter: D3D11 Device created (Feature Level: " + 
                 std::to_string((featureLevel >> 12) & 0xF) + "." + 
                 std::to_string((featureLevel >> 8) & 0xF) + ")");
    
    return true;
}

bool DXGIPresenter::CheckTearingSupport() {
    BOOL allowTearing = FALSE;
    
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(m_factory.As(&factory5))) {
        HRESULT hr = factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allowTearing,
            sizeof(allowTearing)
        );
        
        if (SUCCEEDED(hr)) {
            m_tearingSupported = (allowTearing == TRUE);
        }
    }
    
    return m_tearingSupported;
}

HWND DXGIPresenter::CreateBorderlessWindow() {
    // Generate unique window class name
    m_windowClassName = kWindowClassName;
    m_windowClassName += L"_" + std::to_wstring(GetCurrentProcessId());

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = m_hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = m_windowClassName.c_str();
    
    if (!RegisterClassExW(&wc)) {
        DWORD error = GetLastError();
        Logger::Error("DXGIPresenter: RegisterClassExW failed: " + std::to_string(error));
        return nullptr;
    }

    // Extended window style for WGC optimization
    // WS_EX_NOREDIRECTIONBITMAP: Critical for efficient WGC capture
    DWORD exStyle = WS_EX_APPWINDOW;
    if (m_config.noRedirectionBitmap) {
        exStyle |= WS_EX_NOREDIRECTIONBITMAP;
    }
    
    // Hidden mode styles (Ghost Window)
    if (m_config.hiddenMode) {
        // Mouse passthrough: WS_EX_TRANSPARENT + WS_EX_LAYERED
        // Makes window click-through so operator can't accidentally interact
        if (m_config.mousePassthrough) {
            exStyle |= WS_EX_TRANSPARENT | WS_EX_LAYERED;
        }
        
        // Hide from taskbar and Alt+Tab
        // WS_EX_TOOLWINDOW: Prevents appearance in taskbar and task switcher
        if (m_config.hideFromTaskbar) {
            exStyle &= ~WS_EX_APPWINDOW;  // Remove from taskbar
            exStyle |= WS_EX_TOOLWINDOW;   // Hide from Alt+Tab
        }
    }

    // Window style: WS_POPUP for borderless
    // MUST keep WS_VISIBLE - required for WGC to capture frames
    DWORD style = WS_POPUP | WS_VISIBLE;
    
    // Determine window position
    // In hidden mode, use virtual coordinates (off-screen)
    int windowX = m_config.windowX;
    int windowY = m_config.windowY;
    if (m_config.hiddenMode) {
        windowX = m_config.virtualX;  // Default: 20000 (off any physical monitor)
        windowY = m_config.virtualY;  // Default: 0
        Logger::Info("DXGIPresenter: Hidden mode enabled - window at virtual coordinates (" +
                     std::to_string(windowX) + ", " + std::to_string(windowY) + ")");
    }
    
    // Create the window
    HWND hwnd = CreateWindowExW(
        exStyle,
        m_windowClassName.c_str(),
        m_config.windowTitle.c_str(),
        style,
        windowX,
        windowY,
        m_config.width,
        m_config.height,
        nullptr,
        nullptr,
        m_hInstance,
        this  // Pass this pointer for WM_CREATE
    );

    if (!hwnd) {
        DWORD error = GetLastError();
        Logger::Error("DXGIPresenter: CreateWindowExW failed: " + std::to_string(error));
        return nullptr;
    }

    // Set topmost if configured (not recommended for hidden mode)
    if (m_config.topmost && !m_config.hiddenMode) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, 
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Disable DWM window animations for lower latency
    BOOL value = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &value, sizeof(value));

    // For Windows 11: Try to disable rounded corners
    DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
    
    // SetWindowDisplayAffinity for privacy/security
    // WDA_EXCLUDEFROMCAPTURE: Hides from OBS, Zoom, PrintScreen, etc.
    // WARNING: May affect vMix capture depending on capture method - test carefully!
    if (m_config.excludeFromCapture) {
        if (SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
            Logger::Info("DXGIPresenter: SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) - window hidden from standard capture");
        } else {
            DWORD error = GetLastError();
            Logger::Warning("DXGIPresenter: SetWindowDisplayAffinity failed: " + std::to_string(error) + 
                           " (requires Windows 10 2004+)");
        }
    }
    
    // For layered windows with transparency, we need to set attributes
    // Using full opacity since we just want click-through, not visual transparency
    if (m_config.mousePassthrough && (exStyle & WS_EX_LAYERED)) {
        // Set full opacity (255) - window is visually opaque but clicks pass through
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
    }

    Logger::Info("DXGIPresenter: Borderless window created at (" + 
                 std::to_string(windowX) + "," + std::to_string(windowY) + 
                 ") size " + std::to_string(m_config.width) + "x" + std::to_string(m_config.height));
    
    if (m_config.hiddenMode) {
        Logger::Info("  Hidden mode: ENABLED");
        Logger::Info("  Mouse passthrough: " + std::string(m_config.mousePassthrough ? "YES" : "NO"));
        Logger::Info("  Hidden from taskbar: " + std::string(m_config.hideFromTaskbar ? "YES" : "NO"));
        Logger::Info("  Excluded from capture: " + std::string(m_config.excludeFromCapture ? "YES" : "NO"));
    }
    
    return hwnd;
}

bool DXGIPresenter::CreateSwapChain() {
    if (!m_window || !m_device) {
        return false;
    }

    // Swap chain description for FLIP_DISCARD
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = m_config.width;
    swapChainDesc.Height = m_config.height;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // Compatible with WGC
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = m_config.bufferCount;
    swapChainDesc.Scaling = DXGI_SCALING_NONE;
    
    // CRITICAL: FLIP_DISCARD for DWM bypass and minimum latency
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    
    // Flags for low latency
    swapChainDesc.Flags = 0;
    if (m_config.enableWaitableObject) {
        swapChainDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }
    if (m_tearingSupported && m_config.enableTearing) {
        swapChainDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    // Create swap chain for HWND
    HRESULT hr = m_factory->CreateSwapChainForHwnd(
        m_device.Get(),
        m_window,
        &swapChainDesc,
        nullptr,    // No fullscreen description
        nullptr,    // No output restriction
        &m_swapChain
    );

    if (FAILED(hr)) {
        Logger::Error("DXGIPresenter: CreateSwapChainForHwnd failed: " + HResultToHex(hr));
        return false;
    }

    // Disable Alt+Enter fullscreen toggle (prevents mode changes)
    m_factory->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER);

    // Configure frame latency waitable object
    if (m_config.enableWaitableObject) {
        ComPtr<IDXGISwapChain2> swapChain2;
        if (SUCCEEDED(m_swapChain.As(&swapChain2))) {
            // Set maximum frame latency to 1 (minimum possible)
            hr = swapChain2->SetMaximumFrameLatency(1);
            if (SUCCEEDED(hr)) {
                m_frameLatencyWaitable = swapChain2->GetFrameLatencyWaitableObject();
                if (m_frameLatencyWaitable) {
                    Logger::Info("DXGIPresenter: Frame latency waitable object created (max latency: 1 frame)");
                }
            }
        }
    }

    Logger::Info("DXGIPresenter: Swap chain created with FLIP_DISCARD");
    Logger::Info("  Buffer count: " + std::to_string(swapChainDesc.BufferCount));
    Logger::Info("  Format: BGRA8_UNORM");
    Logger::Info("  Flags: 0x" + std::to_string(swapChainDesc.Flags));
    
    return true;
}

bool DXGIPresenter::CreateRenderTarget() {
    if (!m_swapChain) {
        return false;
    }

    // Get backbuffer
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&m_backBuffer));
    if (FAILED(hr)) {
        Logger::Error("DXGIPresenter: GetBuffer failed: " + HResultToHex(hr));
        return false;
    }

    // Create render target view
    hr = m_device->CreateRenderTargetView(m_backBuffer.Get(), nullptr, &m_rtv);
    if (FAILED(hr)) {
        Logger::Error("DXGIPresenter: CreateRenderTargetView failed: " + HResultToHex(hr));
        return false;
    }

    // Set the render target
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);

    // Set viewport
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = static_cast<float>(m_config.width);
    viewport.Height = static_cast<float>(m_config.height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &viewport);

    Logger::Info("DXGIPresenter: Render target created");
    return true;
}

bool DXGIPresenter::WaitForVBlank() {
    if (m_frameLatencyWaitable) {
        DWORD result = WaitForSingleObjectEx(m_frameLatencyWaitable, 1000, TRUE);
        if (result == WAIT_OBJECT_0) {
            return true;
        } else if (result == WAIT_TIMEOUT) {
            Logger::Warning("DXGIPresenter: Frame latency wait timeout");
            return false;
        }
    }
    return true;  // No waitable object, proceed immediately
}

bool DXGIPresenter::Present() {
    if (!m_swapChain || !m_initialized.load(std::memory_order_acquire)) {
        return false;
    }

    LARGE_INTEGER startTime;
    QueryPerformanceCounter(&startTime);

    // Determine present parameters
    UINT syncInterval = 0;  // No VSync by default for minimum latency
    UINT presentFlags = 0;

    if (m_tearingSupported && m_config.enableTearing) {
        // Tearing mode: sync interval 0 + ALLOW_TEARING flag
        presentFlags = DXGI_PRESENT_ALLOW_TEARING;
        syncInterval = 0;
    } else if (!m_config.vsync) {
        // No VSync, no tearing support: just present immediately
        syncInterval = 0;
    } else {
        // VSync enabled
        syncInterval = 1;
    }

    HRESULT hr = m_swapChain->Present(syncInterval, presentFlags);
    
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            Logger::Error("DXGIPresenter: Device lost - requires recreation");
            m_initialized.store(false, std::memory_order_release);
            return false;
        }
        
        if (hr == DXGI_STATUS_OCCLUDED) {
            // Window minimized or occluded, not an error
            return true;
        }
        
        Logger::Warning("DXGIPresenter: Present failed: " + HResultToHex(hr));
        return false;
    }

    // Calculate frame latency
    LARGE_INTEGER endTime;
    QueryPerformanceCounter(&endTime);
    
    if (m_lastPresentTime.QuadPart > 0) {
        float latencyMs = static_cast<float>(endTime.QuadPart - m_lastPresentTime.QuadPart) * 
                          1000.0f / m_perfFrequency.QuadPart;
        m_lastFrameLatencyMs.store(latencyMs, std::memory_order_relaxed);
    }
    m_lastPresentTime = endTime;
    
    m_presentCount.fetch_add(1, std::memory_order_relaxed);
    
    return true;
}

bool DXGIPresenter::Resize(int width, int height) {
    if (!m_swapChain) {
        return false;
    }

    // Release old render target
    m_rtv.Reset();
    m_backBuffer.Reset();
    m_context->ClearState();

    // Resize buffers
    HRESULT hr = m_swapChain->ResizeBuffers(
        m_config.bufferCount,
        width,
        height,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        m_config.enableWaitableObject ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0
    );

    if (FAILED(hr)) {
        Logger::Error("DXGIPresenter: ResizeBuffers failed: " + HResultToHex(hr));
        return false;
    }

    m_config.width = width;
    m_config.height = height;

    // Recreate render target
    return CreateRenderTarget();
}

LRESULT CALLBACK DXGIPresenter::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Store this pointer for later access
            CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return 0;
        }
        
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
            
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        
        case WM_ERASEBKGND:
            return 1;  // Don't erase background
            
        case WM_SIZE:
            // Could trigger resize here if needed
            return 0;
            
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                // ESC to close (optional, can be removed in production)
                DestroyWindow(hwnd);
                return 0;
            }
            break;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
