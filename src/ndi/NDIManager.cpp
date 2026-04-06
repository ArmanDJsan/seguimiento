/**
 * NDIManager.cpp
 * * Implementación del gestor de envío NDI optimizada para baja latencia.
 * * Cambios realizados:
 * 1. Eliminación de NDIlib_send_set_video_async_completion (No existe en Windows SDK 6).
 * 2. Uso de NDIlib_send_send_video_async_v2 para envío no bloqueante.
 * 3. Gestión de memoria pinned (CUDA) para transferencia ultra rápida GPU->CPU.
 * 4. Implementación de Timecode sintetizado para sincronización en vMix.
 */

#include "NDIManager.h"
#include "../utils/Logger.h"

#include <algorithm>
#include <cstring>

 // El SDK 6 de NDI es obligatorio. Se asume instalado en las rutas del proyecto.
#include <Processing.NDI.Lib.h>

namespace {
    // Configuración para 4K @ 29.97fps (Estándar NTSC para vMix)
    constexpr int kFrameRateNumerator = 30000;
    constexpr int kFrameRateDenominator = 1001;
    constexpr float kAspectRatio = 16.0f / 9.0f;

    // Bytes por píxel según formato
    constexpr size_t kBytesPerPixelUYVY = 2;   // YCbCr 4:2:2 (Nativo DeckLink)
    constexpr size_t kBytesPerPixelBGRA = 4;   // 32-bit con Alpha
}

// Destructor de NDIChannel: Limpieza de recursos por canal
NDIManager::NDIChannel::~NDIChannel() {
    if (sender) {
        NDIlib_send_destroy(sender);
        sender = nullptr;
    }

    if (transferComplete) {
        cudaEventDestroy(transferComplete);
        transferComplete = nullptr;
    }

    if (pinnedBuffer) {
        cudaFreeHost(pinnedBuffer);
        pinnedBuffer = nullptr;
    }
}

NDIManager::NDIManager() : m_initialized(false) {
    Logger::Info("NDIManager: Instancia creada.");
}

NDIManager::~NDIManager() {
    ReleaseAll();
}

bool NDIManager::Initialize() {
    if (m_initialized) return true;

    if (!NDIlib_initialize()) {
        Logger::Error("NDIManager: Error al inicializar la librería NDI.");
        return false;
    }

    Logger::Info("NDIManager: Librería NDI 6 inicializada correctamente.");
    m_initialized = true;
    return true;
}

bool NDIManager::CreateSender(int channelID, const std::string& senderName,
    unsigned int width, unsigned int height,
    bool useUYVY) {
    if (!m_initialized) {
        Logger::Error("NDIManager: No inicializado. Llama a Initialize() primero.");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_channelsMutex);

    if (m_channels.find(channelID) != m_channels.end()) {
        Logger::Warning("NDIManager: El emisor para el canal " + std::to_string(channelID) + " ya existe.");
        return true;
    }

    auto channel = std::make_unique<NDIChannel>();
    channel->name = senderName;
    channel->width = width;
    channel->height = height;
    channel->useUYVY = useUYVY;
    channel->isActive = false;

    size_t bytesPerPixel = useUYVY ? kBytesPerPixelUYVY : kBytesPerPixelBGRA;
    size_t bufferSize = static_cast<size_t>(width) * height * bytesPerPixel;

    // Reservar memoria Pinned (Host) para transferencias DMA desde la GPU
    if (!AllocatePinnedBuffer(*channel, bufferSize)) {
        Logger::Error("NDIManager: Error al reservar memoria Pinned para canal " + std::to_string(channelID));
        return false;
    }

    // Evento CUDA para sincronizar la copia GPU -> RAM
    cudaError_t cudaErr = cudaEventCreateWithFlags(&channel->transferComplete, cudaEventDisableTiming);
    if (cudaErr != cudaSuccess) {
        Logger::Error("NDIManager: Error CUDA Event: " + std::string(cudaGetErrorString(cudaErr)));
        return false;
    }

    // Configuración del emisor NDI
    NDIlib_send_create_t sendDesc;
    sendDesc.p_ndi_name = senderName.c_str();
    sendDesc.p_groups = nullptr;
    sendDesc.clock_video = true;  // NDI gestiona el timing del flujo
    sendDesc.clock_audio = false;

    channel->sender = NDIlib_send_create(&sendDesc);
    if (!channel->sender) {
        Logger::Error("NDIManager: Error al crear sender NDI para: " + senderName);
        return false;
    }

    Logger::Info("NDIManager: Emisor creado -> " + senderName + " (" + std::to_string(width) + "x" + std::to_string(height) + ")");

    channel->isActive = true;
    m_channels[channelID] = std::move(channel);

    return true;
}

bool NDIManager::SendFrameInternal(int channelID, void* cudaBuffer,
    unsigned int width, unsigned int height,
    cudaStream_t stream, bool useUYVY) {
    // Lock scope 1: Check channel exists and get reference
    NDIChannel* pChannel = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_channelsMutex);
        auto it = m_channels.find(channelID);
        if (it == m_channels.end() || !it->second->isActive) return false;
        pChannel = it->second.get();
    }

    NDIChannel& channel = *pChannel;

    // Evitar solapamiento de frames si el bus PCIe está saturado
    if (channel.frameInFlight.load(std::memory_order_acquire)) {
        return true;
    }

    size_t bytesPerPixel = useUYVY ? kBytesPerPixelUYVY : kBytesPerPixelBGRA;
    size_t lineStride = static_cast<size_t>(width) * bytesPerPixel;
    size_t bufferSize = lineStride * height;

    if (!AllocatePinnedBuffer(channel, bufferSize)) return false;

    // 1. Copia asíncrona de GPU a Memoria Pinned (CPU)
    cudaMemcpyAsync(channel.pinnedBuffer, cudaBuffer, bufferSize, cudaMemcpyDeviceToHost, stream);
    cudaEventRecord(channel.transferComplete, stream);

    // 2. Sincronización mínima: Esperar a que los datos estén en RAM antes de que NDI los lea
    cudaEventSynchronize(channel.transferComplete);

    // 3. Preparar el frame de NDI
    NDIlib_video_frame_v2_t ndiFrame;
    ndiFrame.xres = static_cast<int>(width);
    ndiFrame.yres = static_cast<int>(height);
    ndiFrame.FourCC = useUYVY ? NDIlib_FourCC_type_UYVY : NDIlib_FourCC_type_BGRA;
    ndiFrame.frame_rate_N = kFrameRateNumerator;
    ndiFrame.frame_rate_D = kFrameRateDenominator;
    ndiFrame.picture_aspect_ratio = kAspectRatio;
    ndiFrame.line_stride_in_bytes = static_cast<int>(lineStride);
    ndiFrame.p_data = static_cast<uint8_t*>(channel.pinnedBuffer);
    ndiFrame.p_metadata = nullptr;

    // Generar timecode automático para vMix
    ndiFrame.timecode = NDIlib_send_timecode_synthesize;

    // 4. Envío asíncrono (NDI hace una copia interna rápida)
    NDIlib_send_send_video_async_v2(channel.sender, &ndiFrame);

    // Liberamos el flag para el siguiente ciclo
    channel.frameInFlight.store(false, std::memory_order_release);

    return true;
}

bool NDIManager::AllocatePinnedBuffer(NDIChannel& channel, size_t size) {
    if (channel.pinnedBuffer && channel.pinnedBufferSize >= size) return true;

    if (channel.pinnedBuffer) {
        cudaFreeHost(channel.pinnedBuffer);
    }

    cudaError_t err = cudaMallocHost(&channel.pinnedBuffer, size);
    if (err != cudaSuccess) {
        Logger::Error("NDIManager: Error en cudaMallocHost: " + std::string(cudaGetErrorString(err)));
        return false;
    }

    channel.pinnedBufferSize = size;
    return true;
}

bool NDIManager::SendUYVYFrame(int channelID, void* cudaUYVYBuffer, unsigned int width, unsigned int height, cudaStream_t stream) {
    return SendFrameInternal(channelID, cudaUYVYBuffer, width, height, stream, true);
}

bool NDIManager::SendBGRAFrame(int channelID, void* cudaBGRABuffer, unsigned int width, unsigned int height, cudaStream_t stream) {
    return SendFrameInternal(channelID, cudaBGRABuffer, width, height, stream, false);
}

void NDIManager::ReleaseSender(int channelID) {
    std::lock_guard<std::mutex> lock(m_channelsMutex);
    auto it = m_channels.find(channelID);
    if (it != m_channels.end()) {
        Logger::Info("NDIManager: Liberando emisor del canal " + std::to_string(channelID));
        m_channels.erase(it);
    }
}

void NDIManager::ReleaseAll() {
    std::lock_guard<std::mutex> lock(m_channelsMutex);
    Logger::Info("NDIManager: Cerrando todos los canales.");
    m_channels.clear();

    if (m_initialized) {
        NDIlib_destroy();
        m_initialized = false;
    }
}

int NDIManager::GetActiveSenderCount() const {
    std::lock_guard<std::mutex> lock(m_channelsMutex);
    return static_cast<int>(m_channels.size());
}