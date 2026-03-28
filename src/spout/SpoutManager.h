/**
 * SpoutManager.h
 * 
 * Manages Spout senders for multiple video channels
 * Sends zero-latency video to vMix via GPU memory sharing
 */

#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cuda_runtime.h>
#include <cuda_d3d11_interop.h>
#include <string>
#include <map>
#include <memory>

// Forward declaration for Spout SDK (actual type resolved in implementation)
class SpoutSenderImpl;

/**
 * Manages multiple Spout senders for video output to vMix
 */
class SpoutManager {
public:
    explicit SpoutManager(ID3D11Device* device);
    ~SpoutManager();
    
    // Initialize Spout for a specific channel
    bool CreateSender(int channelID, const std::string& senderName, 
                      unsigned int width, unsigned int height);
    
    // Copy CUDA BGRA buffer directly into shared DX11 texture (zero-copy GPU path)
    bool CopyCudaToSharedTexture(int channelID, void* cudaBGRABuffer,
                                 unsigned int width, unsigned int height,
                                 cudaStream_t stream);
    
    // Send a texture to vMix via Spout
    bool SendTexture(int channelID, ID3D11Texture2D* texture);
    bool SendTexture(int channelID); // Convenience: uses internally managed texture
    
    // Access shared texture for diagnostics / downstream consumers
    ID3D11Texture2D* GetSharedTexture(int channelID) const;
    
    // Release a specific sender
    void ReleaseSender(int channelID);
    
    // Release all senders
    void ReleaseAll();
    
private:
    struct SpoutChannel {
        std::unique_ptr<SpoutSenderImpl> sender;
        std::string name;
        unsigned int width;
        unsigned int height;
        bool isActive;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture;
        cudaGraphicsResource* cudaResource = nullptr;
    };
    
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    std::map<int, std::unique_ptr<SpoutChannel>> m_channels;
};
