/**
 * SpoutManager.cpp
 * 
 * Implementation of Spout video sender management
 */

#include "SpoutManager.h"
#include "../utils/Logger.h"
#include <sstream>

// Attempt to use real Spout SDK if available; otherwise fall back to no-op stub
#if __has_include("SpoutDirectX.h")
    #define HAS_SPOUT_DIRECTX 1
    #define HAS_SPOUT_SENDER 0
    #include "SpoutDirectX.h"
    using SpoutSenderImpl = spoutDirectX;
#elif __has_include("SpoutSender.h")
    #define HAS_SPOUT_DIRECTX 0
    #define HAS_SPOUT_SENDER 1
    #include "SpoutSender.h"
    using SpoutSenderImpl = SpoutSender;
#else
    #define HAS_SPOUT_DIRECTX 0
    #define HAS_SPOUT_SENDER 0
    // Minimal stub to keep the build working without the SDK
    class SpoutSenderStub {
    public:
        bool CreateSender(const char* senderName, unsigned int width, unsigned int height) { 
            (void)senderName; (void)width; (void)height;
            return true;
        }
        bool SendTexture(ID3D11Texture2D* texture, unsigned int width, unsigned int height) { 
            (void)texture; (void)width; (void)height;
            return true;
        }
        void ReleaseSender() {}
    };
    using SpoutSenderImpl = SpoutSenderStub;
#endif

namespace {
constexpr size_t kBytesPerPixelBGRA = 4;

void UnregisterCudaResource(cudaGraphicsResource*& resource, const std::string& channelLabel) {
    if (!resource) {
        return;
    }
    cudaError_t status = cudaGraphicsUnregisterResource(resource);
    if (status != cudaSuccess) {
        Logger::Error("Failed to unregister CUDA resource for " + channelLabel + ": " +
                      std::string(cudaGetErrorString(status)));
    }
    resource = nullptr;
}
} // namespace

SpoutManager::SpoutManager(ID3D11Device* device) {
    if (device) {
        m_device = device;
        device->GetImmediateContext(m_context.ReleaseAndGetAddressOf());
    }
#if !HAS_SPOUT_DIRECTX && !HAS_SPOUT_SENDER
    static bool stubWarned = false;
    if (!stubWarned) {
        Logger::Warning("Spout SDK not detected; using stub sender (textures will not be published).");
        stubWarned = true;
    }
#endif
    Logger::Info("SpoutManager initialized");
}

SpoutManager::~SpoutManager() {
    ReleaseAll();
}

bool SpoutManager::CreateSender(int channelID, const std::string& senderName,
                                unsigned int width, unsigned int height) {
    if (!m_device) {
        Logger::Error("Cannot create Spout sender without a valid D3D11 device");
        return false;
    }

    Logger::Info("Creating Spout sender: " + senderName + " (" +
                 std::to_string(width) + "x" + std::to_string(height) + ")");
    
    // Check if sender already exists
    if (m_channels.find(channelID) != m_channels.end()) {
        Logger::Warning("Spout sender already exists for channel " + std::to_string(channelID));
        return false;
    }
    
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // Required for vMix compatibility
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture;
    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, sharedTexture.GetAddressOf());
    if (FAILED(hr)) {
        std::stringstream ss;
        ss << "Failed to create shared texture for Spout sender. HRESULT: 0x"
           << std::hex << hr;
        Logger::Error(ss.str());
        return false;
    }

    auto channel = std::make_unique<SpoutChannel>();
    channel->name = senderName;
    channel->width = width;
    channel->height = height;
    channel->isActive = false;
    channel->sharedTexture = sharedTexture;

    cudaError_t registerStatus = cudaGraphicsD3D11RegisterResource(
        &channel->cudaResource,
        channel->sharedTexture.Get(),
        cudaGraphicsRegisterFlagsNone);
    if (registerStatus != cudaSuccess) {
        Logger::Error("cudaGraphicsD3D11RegisterResource failed for sender " + senderName + ": " +
                      std::string(cudaGetErrorString(registerStatus)));
        return false;
    }

    channel->sender = std::make_unique<SpoutSenderImpl>();
    if (!channel->sender->CreateSender(senderName.c_str(), width, height)) {
        Logger::Error("Failed to initialize Spout sender: " + senderName);
        UnregisterCudaResource(channel->cudaResource, "sender " + senderName);
        return false;
    }

    cudaError_t evtStatus = cudaEventCreateWithFlags(&channel->copyCompleteEvent, cudaEventDisableTiming);
    if (evtStatus != cudaSuccess) {
        Logger::Error("Failed to create CUDA event for sender " + senderName + ": " +
                      std::string(cudaGetErrorString(evtStatus)));
        UnregisterCudaResource(channel->cudaResource, "sender " + senderName);
        return false;
    }
    channel->isActive = true;

    m_channels[channelID] = std::move(channel);
    Logger::Info("Spout sender created successfully: " + senderName);
    return true;
}

bool SpoutManager::CopyCudaToSharedTexture(int channelID, void* cudaBGRABuffer,
                                           unsigned int width, unsigned int height,
                                           cudaStream_t stream) {
    if (!cudaBGRABuffer) {
        Logger::Error("CUDA BGRA buffer is null for channel " + std::to_string(channelID));
        return false;
    }

    if (width == 0 || height == 0) {
        Logger::Error("Invalid dimensions for CUDA/DX11 copy");
        return false;
    }

    auto it = m_channels.find(channelID);
    if (it == m_channels.end()) {
        Logger::Warning("Spout channel not found for channel " + std::to_string(channelID));
        return false;
    }

    auto& channel = *it->second;
    if (!channel.cudaResource || !channel.sharedTexture) {
        Logger::Error("Spout channel not initialized for CUDA/DX11 interop");
        return false;
    }

    if (channel.width != width || channel.height != height) {
        Logger::Warning("Dimension mismatch when copying to shared texture for channel " +
                        std::to_string(channelID));
        return false;
    }

    if (!channel.isActive) {
        Logger::Error("Spout channel " + channel.name + " is not active");
        return false;
    }

    cudaError_t status = cudaGraphicsMapResources(1, &channel.cudaResource, stream);
    if (status != cudaSuccess) {
        Logger::Error("Failed to map shared texture for CUDA: " + std::string(cudaGetErrorString(status)));
        return false;
    }

    cudaArray_t array = nullptr;
    status = cudaGraphicsSubResourceGetMappedArray(&array, channel.cudaResource, 0, 0);
    if (status != cudaSuccess) {
        Logger::Error("Failed to get mapped array: " + std::string(cudaGetErrorString(status)));
        cudaGraphicsUnmapResources(1, &channel.cudaResource, stream);
        return false;
    }

    const size_t pitch = static_cast<size_t>(width) * kBytesPerPixelBGRA; // Row pitch in bytes for BGRA texture
    status = cudaMemcpy2DToArrayAsync(
        array,
        0,
        0,
        cudaBGRABuffer,
        pitch,
        pitch,
        height,
        cudaMemcpyDeviceToDevice,
        stream);
    if (status != cudaSuccess) {
        Logger::Error("cudaMemcpy2DToArrayAsync failed: " + std::string(cudaGetErrorString(status)));
        cudaGraphicsUnmapResources(1, &channel.cudaResource, stream);
        return false;
    }

    status = cudaEventRecord(channel.copyCompleteEvent, stream);
    if (status != cudaSuccess) {
        Logger::Error("Failed to record CUDA event for channel " + channel.name + ": " +
                      std::string(cudaGetErrorString(status)));
        cudaGraphicsUnmapResources(1, &channel.cudaResource, stream);
        return false;
    }

    status = cudaGraphicsUnmapResources(1, &channel.cudaResource, stream);
    if (status != cudaSuccess) {
        Logger::Error("Failed to unmap shared texture: " + std::string(cudaGetErrorString(status)));
        return false;
    }

    return true;
}

bool SpoutManager::SendTexture(int channelID, ID3D11Texture2D* texture) {
    auto it = m_channels.find(channelID);
    if (it == m_channels.end() || !it->second->isActive) {
        return false;
    }
    
    if (!texture) {
        return false;
    }

    // Ensure GPU copy into shared texture is complete to avoid tearing
    cudaError_t waitStatus = cudaEventSynchronize(it->second->copyCompleteEvent);
    if (waitStatus != cudaSuccess) {
        Logger::Error("Failed to sync CUDA copy for channel " + it->second->name + ": " +
                      std::string(cudaGetErrorString(waitStatus)));
        return false;
    }

#if HAS_SPOUT_DIRECTX
    return it->second->sender->SendTexture(texture);
#elif HAS_SPOUT_SENDER
    return it->second->sender->SendTexture(texture, it->second->width, it->second->height);
#else
    // Stub path when Spout SDK is not available during compilation
    (void)texture;
    return true;
#endif
}

bool SpoutManager::SendTexture(int channelID) {
    auto it = m_channels.find(channelID);
    if (it == m_channels.end()) {
        return false;
    }
    return SendTexture(channelID, it->second->sharedTexture.Get());
}

ID3D11Texture2D* SpoutManager::GetSharedTexture(int channelID) const {
    auto it = m_channels.find(channelID);
    if (it == m_channels.end() || !it->second->sharedTexture) {
        return nullptr;
    }
    return it->second->sharedTexture.Get();
}

void SpoutManager::ReleaseSender(int channelID) {
    auto it = m_channels.find(channelID);
    if (it != m_channels.end()) {
        Logger::Info("Releasing Spout sender: " + it->second->name);
        
        UnregisterCudaResource(it->second->cudaResource, "channel " + it->second->name);
        if (it->second->copyCompleteEvent) {
            cudaEventDestroy(it->second->copyCompleteEvent);
            it->second->copyCompleteEvent = nullptr;
        }

        if (it->second->sender) {
            it->second->sender->ReleaseSender();
            it->second->sender.reset();
        }
        
        m_channels.erase(it);
    }
}

void SpoutManager::ReleaseAll() {
    Logger::Info("Releasing all Spout senders");
    
    for (auto& pair : m_channels) {
        UnregisterCudaResource(pair.second->cudaResource, "channel " + pair.second->name);
        if (pair.second->copyCompleteEvent) {
            cudaEventDestroy(pair.second->copyCompleteEvent);
            pair.second->copyCompleteEvent = nullptr;
        }
        if (pair.second->sender) {
            pair.second->sender->ReleaseSender();
            pair.second->sender.reset();
        }
    }
    
    m_channels.clear();
}
