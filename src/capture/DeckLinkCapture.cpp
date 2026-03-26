/**
 * DeckLinkCapture.cpp
 * 
 * Implementation of zero-copy DMA capture from Blackmagic DeckLink cards
 * Uses custom memory allocator for direct GPU memory access
 */

#include "DeckLinkCapture.h"
#include "../utils/Logger.h"

DeckLinkCapture::DeckLinkCapture(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_device(device)
    , m_context(context)
    , m_allocator(nullptr)
{
    if (m_device) {
        m_device->AddRef();
    }
    if (m_context) {
        m_context->AddRef();
    }
    
    // Initialize channel structure
    m_channel.deckLinkInput = nullptr;
    m_channel.sharedTexture = nullptr;
    m_channel.yuvTexture = nullptr;
    m_channel.rgbTexture = nullptr;
    m_channel.channelID = -1;
    m_channel.width = 3840;  // Default 4K
    m_channel.height = 2160;
    m_channel.isActive = false;
}

DeckLinkCapture::~DeckLinkCapture() {
    Stop();
    
    // Release DirectX resources
    if (m_channel.sharedTexture) {
        m_channel.sharedTexture->Release();
    }
    if (m_channel.yuvTexture) {
        m_channel.yuvTexture->Release();
    }
    if (m_channel.rgbTexture) {
        m_channel.rgbTexture->Release();
    }
    
    if (m_device) {
        m_device->Release();
    }
    if (m_context) {
        m_context->Release();
    }
}

bool DeckLinkCapture::Initialize(int deviceIndex, const std::string& channelName) {
    Logger::Info("Initializing DeckLink capture for device " + std::to_string(deviceIndex));
    
    m_channel.channelName = channelName;
    m_channel.channelID = deviceIndex;
    
    // TODO: Initialize Blackmagic SDK
    // 1. Create IDeckLink interface
    // 2. Query for IDeckLinkInput
    // 3. Set video input format (bmdModeHD1080p60 or bmdMode4K2160p60)
    // 4. Create custom allocator
    // 5. Set frame callback
    
    // Create textures for video processing
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = m_channel.width;
    texDesc.Height = m_channel.height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;  // Enable sharing with Spout
    
    HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_channel.rgbTexture);
    if (FAILED(hr)) {
        Logger::Error("Failed to create RGB texture");
        return false;
    }
    
    // Create YUV input texture
    texDesc.Format = DXGI_FORMAT_R8G8_UNORM;  // YUV 4:2:2 format
    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_channel.yuvTexture);
    if (FAILED(hr)) {
        Logger::Error("Failed to create YUV texture");
        return false;
    }
    
    // Initialize custom allocator
    m_allocator = std::make_unique<CustomAllocator>(m_device);
    
    Logger::Info("DeckLink capture initialized successfully: " + channelName);
    return true;
}

bool DeckLinkCapture::Start() {
    if (!m_channel.deckLinkInput) {
        Logger::Error("Cannot start capture: DeckLink input not initialized");
        return false;
    }
    
    Logger::Info("Starting capture on channel: " + m_channel.channelName);
    
    // TODO: Start capture
    // m_channel.deckLinkInput->StartStreams();
    
    m_channel.isActive = true;
    return true;
}

void DeckLinkCapture::Stop() {
    if (!m_channel.isActive) {
        return;
    }
    
    Logger::Info("Stopping capture on channel: " + m_channel.channelName);
    
    // TODO: Stop capture
    // if (m_channel.deckLinkInput) {
    //     m_channel.deckLinkInput->StopStreams();
    // }
    
    m_channel.isActive = false;
}

void DeckLinkCapture::OnFrameArrived(IDeckLinkVideoInputFrame* videoFrame) {
    if (!videoFrame || !m_channel.isActive) {
        return;
    }
    
    // Get frame data pointer
    void* frameBuffer = nullptr;
    // videoFrame->GetBytes(&frameBuffer);
    
    // Upload to GPU texture using UpdateSubresource
    // This is the zero-copy path - data goes directly from DeckLink to GPU
    // m_context->UpdateSubresource(m_channel.yuvTexture, 0, nullptr, 
    //                              frameBuffer, m_channel.width * 2, 0);
    
    // Note: Pixel shader conversion from YUV to RGB happens in rendering pipeline
    // The shader is applied when we render to rgbTexture
}

int DeckLinkCapture::EnumerateDevices() {
    // TODO: Enumerate DeckLink devices
    // Use IDeckLinkIterator to find all available devices
    Logger::Info("Enumerating DeckLink devices...");
    
    // Placeholder: return number of detected devices
    return 0;
}

// CustomAllocator implementation
DeckLinkCapture::CustomAllocator::CustomAllocator(ID3D11Device* device)
    : m_device(device)
{
}

void* DeckLinkCapture::CustomAllocator::AllocateBuffer(unsigned int bufferSize) {
    // Allocate pinned memory that GPU can access directly
    // This is the key to zero-copy performance
    
    // TODO: Allocate GPU-visible memory
    // On Windows, use VirtualAlloc with PAGE_READWRITE | PAGE_NOCACHE
    // Or use DirectX 11 staging textures with CPU read access
    
    void* buffer = nullptr;
    // buffer = VirtualAlloc(nullptr, bufferSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    
    if (buffer) {
        m_pinnedBuffers.push_back(buffer);
    }
    
    return buffer;
}

void DeckLinkCapture::CustomAllocator::ReleaseBuffer(void* buffer) {
    if (!buffer) {
        return;
    }
    
    // TODO: Free the buffer
    // VirtualFree(buffer, 0, MEM_RELEASE);
    
    // Remove from tracked buffers
    auto it = std::find(m_pinnedBuffers.begin(), m_pinnedBuffers.end(), buffer);
    if (it != m_pinnedBuffers.end()) {
        m_pinnedBuffers.erase(it);
    }
}
