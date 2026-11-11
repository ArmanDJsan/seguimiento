/**
 * RankingPublisher.cpp
 *
 * Implementacion del publisher UDP por punto de medicion (contrato TD).
 */

#include "RankingPublisher.h"
#include "../utils/Logger.h"
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

RankingPublisher::RankingPublisher(const RankingPublisherConfig& config)
    : m_config(config)
    , m_initialized(false)
    , m_publishCount(0)
    , m_failCount(0)
    , m_socket(INVALID_SOCKET)
    , m_winsockInitialized(false)
{
    m_target = sockaddr_in{};
}

RankingPublisher::~RankingPublisher() {
    Shutdown();
}

bool RankingPublisher::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_initialized) {
        return true;
    }

    if (!m_config.enabled) {
        Logger::Info("RankingPublisher: deshabilitado por configuracion");
        m_initialized = true;
        return true;
    }

    if (m_config.port == 0) {
        SetLastError("animation.port no configurado (0)");
        Logger::Error("RankingPublisher: " + m_lastError);
        return false;
    }

    WSADATA wsaData{};
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        SetLastError("Winsock initialization failed: " + std::to_string(result));
        Logger::Error("RankingPublisher: " + m_lastError);
        return false;
    }
    m_winsockInitialized = true;

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        SetLastError("Failed to create UDP socket: " + std::to_string(WSAGetLastError()));
        Logger::Error("RankingPublisher: " + m_lastError);
        WSACleanup();
        m_winsockInitialized = false;
        return false;
    }

    m_target = sockaddr_in{};
    m_target.sin_family = AF_INET;
    m_target.sin_port = htons(m_config.port);

    if (inet_pton(AF_INET, m_config.host.c_str(), &m_target.sin_addr) != 1) {
        SetLastError("Host UDP invalido: " + m_config.host);
        Logger::Error("RankingPublisher: " + m_lastError);
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        WSACleanup();
        m_winsockInitialized = false;
        return false;
    }

    m_initialized = true;
    Logger::Info("RankingPublisher: inicializado, destino=" + m_config.host +
                 ":" + std::to_string(m_config.port) +
                 " (UDP, 1 datagrama por punto de medicion)");
    return true;
}

void RankingPublisher::Shutdown() {
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
        Logger::Info("RankingPublisher: shutdown. Enviados: " +
                     std::to_string(m_publishCount.load()) + ", Fallidos: " +
                     std::to_string(m_failCount.load()) + ", seq=" +
                     std::to_string(m_state.seq));
    }
    m_initialized = false;
}

bool RankingPublisher::PublishMeasurePoint(int punto, int64_t frameEpochMs,
                                             const std::vector<int>& posiciones) {
    // El ESTADO se actualiza siempre: el overlay local no depende del UDP. Si el
    // UDP esta apagado (o el socket fallo), el punto igual llega al overlay.
    std::string payload;
    std::string seqTexto;
    PublishSink sink;
    SOCKET sock = INVALID_SOCKET;
    sockaddr_in destino{};
    bool enviarPorUdp = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_config.enabled && !m_sink) {
            return false;
        }

        m_state.Update(posiciones);
        m_state.seq += 1;
        payload = m_state.BuildPayload();
        seqTexto = std::to_string(m_state.seq);
        sink = m_sink;

        if (m_config.enabled) {
            if (!m_initialized || m_socket == INVALID_SOCKET) {
                SetLastError("Publisher no inicializado");
                m_failCount++;
            } else {
                sock = m_socket;
                destino = m_target;
                enviarPorUdp = true;
            }
        }
    }

    // 1) Overlay local: se empuja SIEMPRE, fuera del lock.
    if (sink) {
        sink(payload);
    }

    // 2) UDP (TouchDesigner / el programa Go): solo si esta habilitado.
    if (!enviarPorUdp) {
        Logger::Info("RankingPublisher: punto=" + std::to_string(punto) +
                     " t=" + std::to_string(frameEpochMs) +
                     " seq=" + seqTexto + " (solo overlay, UDP apagado) -> " + payload);
        return true;
    }

    // Un solo datagrama por punto: nunca se parte el JSON en dos envios.
    const int sent = sendto(sock,
                            payload.c_str(),
                            static_cast<int>(payload.size()),
                            0,
                            reinterpret_cast<const sockaddr*>(&destino),
                            sizeof(destino));

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
    Logger::Info("RankingPublisher: punto=" + std::to_string(punto) +
                 " t=" + std::to_string(frameEpochMs) +
                 " seq=" + seqTexto + " -> " + payload);
    return true;
}

std::string RankingPublisher::BuildCurrentPayload() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state.BuildPayload();
}

std::string RankingPublisher::GetLastError() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastError;
}

void RankingPublisher::SetLastError(const std::string& error) {
    m_lastError = error;
    Logger::Warning("RankingPublisher: " + error);
}
