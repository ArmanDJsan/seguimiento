/**
 * SpoutManager.h - FIXED
 */
#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <cuda_runtime.h>
#include <cuda_d3d11_interop.h>
#include <string>
#include <map>
#include <memory>

class SpoutManager {
public:
    explicit SpoutManager(ID3D11Device* device);
    ~SpoutManager();

    bool CreateSender(int channelID, const std::string& senderName,
        unsigned int width, unsigned int height);

    bool CopyCudaToSharedTexture(int channelID, void* cudaBGRABuffer,
        unsigned int width, unsigned int height,
        cudaStream_t stream);

    bool SendTexture(int channelID, ID3D11Texture2D* texture);
    bool SendTexture(int channelID);

    ID3D11Texture2D* GetSharedTexture(int channelID) const;
    void ReleaseSender(int channelID);
    void ReleaseAll();

private:
    struct SpoutChannel {
        // Usamos void* con un deleter personalizado para evitar conflictos de tipos
        std::unique_ptr<void, void(*)(void*)> sender{ nullptr, [](void*) {} };
        std::string name;
        unsigned int width;
        unsigned int height;
        bool isActive;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture;
        cudaGraphicsResource* cudaResource = nullptr;
        cudaEvent_t copyCompleteEvent = nullptr;
    };

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    std::map<int, std::unique_ptr<SpoutChannel>> m_channels;
};