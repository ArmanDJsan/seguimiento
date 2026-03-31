/**
 * SpoutManager.h
 * * Manages Spout senders for multiple video channels
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

 // Forward declaration: Usamos struct para evitar el warning C4099 en Windows
struct SpoutSenderImpl;

/**
 * Manages multiple Spout senders for video output to vMix
 */
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
        std::unique_ptr<SpoutSenderImpl> sender;
        std::string name;
        unsigned int width;
        unsigned int height;
        bool isActive;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sharedTexture;
        cudaGraphicsResource* cudaResource = nullptr;
        cudaEvent_t copyCompleteEvent = nullptr;

        // Destructor explícito para manejar el tipo incompleto del unique_ptr
        SpoutChannel();
        ~SpoutChannel();
    };

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    std::map<int, std::unique_ptr<SpoutChannel>> m_channels;
};