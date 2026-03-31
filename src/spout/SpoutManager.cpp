/**
 * SpoutManager.cpp - FIXED
 */
#include "SpoutManager.h"
#include "../utils/Logger.h"
#include <sstream>

 // Detección automática del SDK
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
class SpoutSenderStub {
public:
    bool CreateSender(const char*, unsigned int, unsigned int) { return true; }
    bool SendTexture(ID3D11Texture2D*, unsigned int = 0, unsigned int = 0) { return true; }
    void ReleaseSender() {}
};
using SpoutSenderImpl = SpoutSenderStub;
#endif

namespace {
    constexpr size_t kBytesPerPixelBGRA = 4;
    void UnregisterCudaResource(cudaGraphicsResource*& resource, const std::string& label) {
        if (!resource) return;
        cudaGraphicsUnregisterResource(resource);
        resource = nullptr;
    }
}

SpoutManager::SpoutManager(ID3D11Device* device) : m_device(device) {
    if (m_device) m_device->GetImmediateContext(m_context.ReleaseAndGetAddressOf());
    Logger::Info("SpoutManager initialized");
}

SpoutManager::~SpoutManager() { ReleaseAll(); }

bool SpoutManager::CreateSender(int channelID, const std::string& senderName, unsigned int width, unsigned int height) {
    if (!m_device || m_channels.count(channelID)) return false;

    D3D11_TEXTURE2D_DESC desc = { width, height, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0},
                                 D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                                 0, D3D11_RESOURCE_MISC_SHARED };

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, tex.GetAddressOf()))) return false;

    auto channel = std::make_unique<SpoutChannel>();
    channel->name = senderName; channel->width = width; channel->height = height;
    channel->sharedTexture = tex;

    if (cudaGraphicsD3D11RegisterResource(&channel->cudaResource, tex.Get(), cudaGraphicsRegisterFlagsNone) != cudaSuccess) return false;

    // Inicialización del Sender con el tipo real
    auto realSender = new SpoutSenderImpl();
    channel->sender = std::unique_ptr<void, void(*)(void*)>(realSender, [](void* p) { delete static_cast<SpoutSenderImpl*>(p); });

    if (!static_cast<SpoutSenderImpl*>(channel->sender.get())->CreateSender(senderName.c_str(), width, height)) return false;

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
    cudaArray_t array;
    cudaGraphicsSubResourceGetMappedArray(&array, ch.cudaResource, 0, 0);

    size_t pitch = (size_t)width * kBytesPerPixelBGRA;
    cudaMemcpy2DToArrayAsync(array, 0, 0, cudaBGRABuffer, pitch, pitch, height, cudaMemcpyDeviceToDevice, stream);
    cudaEventRecord(ch.copyCompleteEvent, stream);
    cudaGraphicsUnmapResources(1, &ch.cudaResource, stream);
    return true;
}

bool SpoutManager::SendTexture(int channelID, ID3D11Texture2D* texture) {
    auto it = m_channels.find(channelID);
    if (it == m_channels.end() || !it->second->isActive) return false;

    cudaEventSynchronize(it->second->copyCompleteEvent);
    auto s = static_cast<SpoutSenderImpl*>(it->second->sender.get());

#if HAS_SPOUT_DIRECTX
    return s->SendTexture(texture);
#else
    return s->SendTexture(texture, it->second->width, it->second->height);
#endif
}

bool SpoutManager::SendTexture(int channelID) {
    auto it = m_channels.find(channelID);
    return it != m_channels.end() ? SendTexture(channelID, it->second->sharedTexture.Get()) : false;
}

void SpoutManager::ReleaseSender(int channelID) {
    auto it = m_channels.find(channelID);
    if (it != m_channels.end()) {
        if (it->second->sender) static_cast<SpoutSenderImpl*>(it->second->sender.get())->ReleaseSender();
        UnregisterCudaResource(it->second->cudaResource, it->second->name);
        if (it->second->copyCompleteEvent) cudaEventDestroy(it->second->copyCompleteEvent);
        m_channels.erase(it);
    }
}

void SpoutManager::ReleaseAll() {
    for (auto const& [id, ch] : m_channels) {
        if (ch->sender) static_cast<SpoutSenderImpl*>(ch->sender.get())->ReleaseSender();
        UnregisterCudaResource(ch->cudaResource, ch->name);
    }
    m_channels.clear();
}

ID3D11Texture2D* SpoutManager::GetSharedTexture(int channelID) const {
    auto it = m_channels.find(channelID);
    return it != m_channels.end() ? it->second->sharedTexture.Get() : nullptr;
}