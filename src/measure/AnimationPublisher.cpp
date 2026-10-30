/**
 * AnimationPublisher.cpp
 *
 * Implementacion del publisher UDP de puntos de medicion.
 * Un solo sendto() por punto (ver AnimationPublisher.h para el contrato).
 */

#include "AnimationPublisher.h"
#include "../utils/Logger.h"
#include <ws2tcpip.h>
#include <sstream>

#pragma comment(lib, "Ws2_32.lib")

AnimationPublisher::AnimationPublisher(const AnimationPublisherConfig& config)
    : m_config(config)
    , m_initialized(false)
    , m_publishCount(0)
    , m_failCount(0)
    , m_socket(INVALID_SOCKET)
    , m_winsockInitialized(false)
{
    m_target = sockaddr_in{};
}

AnimationPublisher::~AnimationPublisher() {
    Shutdown();
}

bool AnimationPublisher::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_initialized) {
        return true;
    }

    if (!m_config.enabled) {
        Logger::Info("AnimationPublisher: deshabilitado por configuracion");
        m_initialized = true;
        return true;
    }

    if (m_config.port == 0) {
        SetLastError("animation.port no configurado (0)");
        Logger::Error("AnimationPublisher: " + m_lastError);
        return false;
    }

    WSADATA wsaData{};
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        SetLastError("Winsock initialization failed: " + std::to_string(result));
        Logger::Error("AnimationPublisher: " + m_lastError);
        return false;
    }
    m_winsockInitialized = true;

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        SetLastError("Failed to create UDP socket: " + std::to_string(WSAGetLastError()));
        Logger::Error("AnimationPublisher: " + m_lastError);
        WSACleanup();
        m_winsockInitialized = false;
        return false;
    }

    m_target = sockaddr_in{};
    m_target.sin_family = AF_INET;
    m_target.sin_port = htons(m_config.port);

    if (inet_pton(AF_INET, m_config.host.c_str(), &m_target.sin_addr) != 1) {
        SetLastError("Host UDP invalido: " + m_config.host);
        Logger::Error("AnimationPublisher: " + m_lastError);
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        WSACleanup();
        m_winsockInitialized = false;
        return false;
    }

    m_initialized = true;
    Logger::Info("AnimationPublisher: inicializado, destino=" + m_config.host +
                 ":" + std::to_string(m_config.port) + " (UDP, 1 datagrama por punto)");
    return true;
}

void AnimationPublisher::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    if (m_winsockInitialized) {
        WSACleanup();
        m_winsockInitialized = false;
    }

    if (m_initialized) {
        Logger::Info("AnimationPublisher: shutdown. Enviados: " +
                     std::to_string(m_publishCount.load()) + ", Fallidos: " +
                     std::to_string(m_failCount.load()));
    }
    m_initialized = false;
}

bool AnimationPublisher::PublishMeasurePoint(int punto, int64_t frameEpochMs,
                                             const std::vector<int>& posiciones) {
    if (!m_config.enabled) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_initialized || m_socket == INVALID_SOCKET) {
        SetLastError("Publisher no inicializado");
        m_failCount++;
        return false;
    }

    const std::string payload = BuildPayload(punto, frameEpochMs, posiciones);

    // Un solo datagrama por punto: nunca se parte el JSON en dos envios.
    int sent = sendto(m_socket,
                      payload.c_str(),
                      static_cast<int>(payload.size()),
                      0,
                      reinterpret_cast<const sockaddr*>(&m_target),
                      sizeof(m_target));

    if (sent == SOCKET_ERROR) {
        SetLastError("sendto failed: " + std::to_string(WSAGetLastError()));
        m_failCount++;
        return false;
    }

    if (static_cast<size_t>(sent) != payload.size()) {
        SetLastError("sendto incompleto: " + std::to_string(sent) + "/" +
                     std::to_string(payload.size()) + " bytes");
        m_failCount++;
        return false;
    }

    m_publishCount++;
    Logger::Info("AnimationPublisher: punto=" + std::to_string(punto) +
                 " t=" + std::to_string(frameEpochMs) + " -> " + payload);
    return true;
}

std::string AnimationPublisher::GetLastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

std::string AnimationPublisher::BuildPayload(int punto, int64_t frameEpochMs,
                                             const std::vector<int>& posiciones) {
    std::ostringstream oss;
    oss << "{\"punto\":" << punto
        << ",\"t\":" << frameEpochMs
        << ",\"posiciones\":[";
    for (size_t i = 0; i < posiciones.size(); ++i) {
        if (i > 0) {
            oss << ",";
        }
        oss << posiciones[i];
    }
    oss << "]}";
    return oss.str();
}

void AnimationPublisher::SetLastError(const std::string& error) {
    m_lastError = error;
    Logger::Warning("AnimationPublisher: " + error);
}
