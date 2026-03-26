/**
 * DeckLinkCapture.h
 * 
 * Custom memory allocator implementation for zero-copy DMA
 * Implements IDeckLinkMemoryAllocator and IDeckLinkVideoInputCallback
 * for direct GPU memory access from Blackmagic DeckLink cards
 */

#pragma once

#include <d3d11.h>
#include <string>
#include <memory>
#include <queue>
#include <mutex>

// Forward declarations for Blackmagic SDK
class IDeckLink;
class IDeckLinkInput;
class IDeckLinkVideoInputFrame;
class IDeckLinkAudioInputPacket;

/**
 * Video channel structure for managing each capture input
 */
struct VideoChannel {
    IDeckLinkInput* deckLinkInput;
    ID3D11Texture2D* sharedTexture;
    ID3D11Texture2D* yuvTexture;
    ID3D11Texture2D* rgbTexture;
    std::string channelName;
    int channelID;
    unsigned int width;
    unsigned int height;
    bool isActive;
};

/**
 * Custom DeckLink capture class with zero-copy memory allocator
 * Philosophy: Maximum performance over development comfort
 */
class DeckLinkCapture {
public:
    DeckLinkCapture(ID3D11Device* device, ID3D11DeviceContext* context);
    ~DeckLinkCapture();
    
    // Initialize capture from specific DeckLink device
    bool Initialize(int deviceIndex, const std::string& channelName);
    
    // Start/Stop capture
    bool Start();
    void Stop();
    
    // Get the RGB texture for Spout/YOLO
    ID3D11Texture2D* GetRGBTexture() const { return m_channel.rgbTexture; }
    
    // Get channel info
    const VideoChannel& GetChannel() const { return m_channel; }
    
    // Static method to enumerate available devices
    static int EnumerateDevices();
    
private:
    // Custom memory allocator implementation
    // Provides GPU memory directly to DeckLink for zero-copy DMA
    class CustomAllocator {
    public:
        CustomAllocator(ID3D11Device* device);
        void* AllocateBuffer(unsigned int bufferSize);
        void ReleaseBuffer(void* buffer);
        
    private:
        ID3D11Device* m_device;
        std::vector<void*> m_pinnedBuffers;
    };
    
    // Frame callback implementation
    void OnFrameArrived(IDeckLinkVideoInputFrame* videoFrame);
    
    // DirectX 11 resources
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_context;
    
    // Channel data
    VideoChannel m_channel;
    
    // Custom allocator
    std::unique_ptr<CustomAllocator> m_allocator;
    
    // Frame queue for async processing
    std::queue<IDeckLinkVideoInputFrame*> m_frameQueue;
    std::mutex m_queueMutex;
};
