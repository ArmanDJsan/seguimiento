/**
 * SpoutManager.cpp
 */

#include "SpoutManager.h"
#include "../utils/Logger.h"
#include <sstream>

 // Definición de la implementación según disponibilidad del SDK
#if __has_include("SpoutDirectX.h")
#include "SpoutDirectX.h"
struct SpoutSenderImpl : public spoutDirectX {};
#define HAS_SPOUT_SDK 1
#elif __has_include("SpoutSender.h")
#include "SpoutSender.h"
struct SpoutSenderImpl : public SpoutSender {};
#define HAS_SPOUT_SDK 1
#else
struct SpoutSenderImpl {
    bool CreateSender(const char*, unsigned int, unsigned int) { return true; }
    bool SendTexture(ID3D11Texture2D*, unsigned int, unsigned int) { return true; }
    void ReleaseSender() {}
};
#define HAS_SPOUT_SDK 0
#endif

// Implementación de constructores de la struct interna para manejar el unique_ptr
SpoutManager::SpoutChannel::SpoutChannel() : isActive(false) {}
SpoutManager::SpoutChannel::~SpoutChannel() = default;

namespace {
    constexpr size_t kBytesPerPixelBGRA = 4;

    void UnregisterCudaResource(cudaGraphicsResource*& resource, const std::string& channelLabel) {
        if (!resource) return;
        cudaError_t status = cudaGraphicsUnregisterResource(resource);
        if (status != cudaSuccess) {
            Logger::Error("Failed to unregister CUDA resource for " + channelLabel + ": " + std::string(cudaGetErrorString(status)));
        }
        resource = nullptr;
    }
}

SpoutManager::SpoutManager(ID3D11Device* device) {
    if (device) {
        m_device = device;
        device->GetImmediateContext(m_context.ReleaseAndGetAddressOf());
    }
    Logger::Info("SpoutManager initialized");
}

SpoutManager::~SpoutManager() {
    ReleaseAll();
}

bool SpoutManager::CreateSender(int channelID, const std::string& senderName, unsigned int width, unsigned int height) {
    if (!m_device) return false;

    auto channel = std::make_unique<SpoutChannel>();
    channel->name = senderName;
    channel->width = width;
    channel->height = height;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, channel->sharedTexture.GetAddressOf());
    if (FAILED(hr)) return false;

    if (cudaGraphicsD3D11RegisterResource(&channel->cudaResource, channel->sharedTexture.Get(), cudaGraphicsRegisterFlagsNone) != cudaSuccess)
        return false;

    // IMPORTANTE: Usar reset(new...) para evitar problemas de visibilidad de tipos en plantillas
    channel->sender.reset(new SpoutSenderImpl());

    if (!channel->sender->CreateSender(senderName.c_str(), width, height)) {
        return false;
    }

    cudaEventCreateWithFlags(&channel->copyCompleteEvent, cudaEventDisableTiming);
    channel->isActive = true;
    m_channels[channelID] = std::move(channel);
    return true;
}

bool SpoutManager::CopyCudaToSharedTexture(int channelID, void* cudaBGRABuffer, unsigned int width, unsigned int height, cudaStream_t stream) {
    auto it = m_channels.find(channelID);
    if (it == m_channels.end() || !it->second->isActive) return false;

    auto& ch = *it->second;
    cudaGraphicsMapResources(1, &ch.cudaResource, stream);
    cudaArray_t array = nullptr;
    cudaGraphicsSubResourceGetMappedArray(&array, ch.cudaResource, 0, 0);

    const size_t pitch = static_cast<size_t>(width) * kBytesPerPixelBGRA;
    cudaMemcpy2DToArrayAsync(array, 0, 0, cudaBGRABuffer, pitch, pitch, height, cudaMemcpyDeviceToDevice, stream);

    cudaEventRecord(ch.copyCompleteEvent, stream);
    cudaGraphicsUnmapResources(1, &ch.cudaResource, stream);
    return true;
}

bool SpoutManager::SendTexture(int channelID, ID3D11Texture2D* texture) {
    auto it = m_channels.find(channelID);
    if (it == m_channels.end() || !it->second->isActive) return false;

    cudaEventSynchronize(it->second->copyCompleteEvent);

#if HAS_SPOUT_SDK
    return it->second->sender->SendTexture(texture, it->second->width, it->second->height);
#else
    return true;
#endif
}

bool SpoutManager::SendTexture(int channelID) {
    auto it = m_channels.find(channelID);
    return it != m_channels.end() ? SendTexture(channelID, it->second->sharedTexture.Get()) : false;
}

void SpoutManager::ReleaseSender(int channelID) {
    auto it = m_channels.find(channelID);
    if (it != m_channels.end()) {
        it->second->sender->ReleaseSender();
        m_channels.erase(it);
    }
}

void SpoutManager::ReleaseAll() {
    for (auto& pair : m_channels) {
        pair.second->sender->ReleaseSender();
    }
    m_channels.clear();
}