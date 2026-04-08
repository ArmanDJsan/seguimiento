/**
 * VideoHubClient.cpp
 *
 * TCP controller for Blackmagic VideoHub devices.
 * Implements minimal routing and labeling commands using the device's
 * text-based protocol.
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "VideoHubClient.h"
#include "../utils/Logger.h"
#include <ws2tcpip.h>
#include <sstream>
#include <vector>
#include <algorithm>
#include <thread>
#include <chrono>

#pragma comment(lib, "Ws2_32.lib")

namespace {
constexpr const char* kRoutingHeader = "VIDEO OUTPUT ROUTING:";
constexpr const char* kInputLabelsHeader = "INPUT LABELS:";
} // namespace

VideoHubClient::VideoHubClient(const std::string& host,
                               uint16_t port,
                               std::unordered_map<std::string, int> inputLookup)
    : m_host(host),
      m_port(port),
      m_socket(INVALID_SOCKET),
      m_winsockInitialized(false),
      m_inputLookup(std::move(inputLookup)),
      m_maxSourceIndex(-1) {
    for (const auto& entry : m_inputLookup) {
        m_maxSourceIndex = std::max(m_maxSourceIndex, entry.second);
    }
}

VideoHubClient::~VideoHubClient() {
    Disconnect();
    if (m_winsockInitialized) {
        WSACleanup();
        m_winsockInitialized = false;
    }
}

bool VideoHubClient::InitializeWinsock() {
    if (m_winsockInitialized) {
        return true;
    }

    WSADATA wsaData{};
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        Logger::Error("[TECH ERROR] WinSock initialization failed: code " + std::to_string(result));
        return false;
    }

    m_winsockInitialized = true;
    return true;
}

bool VideoHubClient::Connect() {
    if (m_socket != INVALID_SOCKET) {
        return true;
    }

    if (!InitializeWinsock()) {
        return false;
    }

    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket == INVALID_SOCKET) {
        LogSocketError("creating VideoHub socket");
        return false;
    }

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(m_port);

    if (inet_pton(AF_INET, m_host.c_str(), &serverAddr.sin_addr) != 1) {
        Logger::Error("[TECH ERROR] Invalid VideoHub IP address: " + m_host);
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return false;
    }

    if (connect(m_socket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        LogSocketError("connecting to VideoHub");
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return false;
    }

    Logger::Info("VideoHub connected at " + m_host + ":" + std::to_string(m_port));
    
    // Consume initial state data sent by VideoHub upon connection
    if (!ConsumeInitialState()) {
        Logger::Warning("Failed to consume VideoHub initial state; continuing anyway");
    }
    
    return true;
}

void VideoHubClient::Disconnect() {
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        Logger::Info("VideoHub connection closed");
    }
}

bool VideoHubClient::IsConnected() const {
    return m_socket != INVALID_SOCKET;
}

bool VideoHubClient::SendCommand(const std::string& payload) {
    if (!IsConnected()) {
        Logger::Error("[TECH ERROR] Attempted to send VideoHub command without connection");
        return false;
    }

    const char* buffer = payload.c_str();
    int totalSent = 0;
    const int totalSize = static_cast<int>(payload.size());

    while (totalSent < totalSize) {
        int sent = send(m_socket, buffer + totalSent, totalSize - totalSent, 0);
        if (sent == SOCKET_ERROR) {
            LogSocketError("sending data to VideoHub");
            return false;
        }
        totalSent += sent;
    }

    return true;
}

bool VideoHubClient::RouteInputToOutput(int outputIndex, int sourceIndex) {
    if (outputIndex < 0 || sourceIndex < 0) {
        Logger::Error("[TECH ERROR] Invalid routing indexes: values must be non-negative (output=" +
                      std::to_string(outputIndex) + ", source=" + std::to_string(sourceIndex) + ")");
        return false;
    }
    if (m_maxSourceIndex >= 0 && sourceIndex > m_maxSourceIndex) {
        Logger::Error("[TECH ERROR] Invalid routing indexes: source exceeds configured range (" +
                      std::to_string(sourceIndex) + " > " + std::to_string(m_maxSourceIndex) + ")");
        return false;
    }
    bool configured = std::any_of(m_inputLookup.begin(), m_inputLookup.end(),
                                  [sourceIndex](const auto& entry) { return entry.second == sourceIndex; });
    if (!configured) {
        Logger::Error("[TECH ERROR] Unknown source index (not configured): " + std::to_string(sourceIndex));
        return false;
    }

    std::ostringstream command;
    command << kRoutingHeader << "\n"
            << outputIndex << " " << sourceIndex << "\n\n";

    return SendCommand(command.str());
}

bool VideoHubClient::RouteInputToOutput(int outputIndex, const std::string& sourceName) {
    auto it = m_inputLookup.find(sourceName);
    if (it == m_inputLookup.end()) {
        Logger::Error("[TECH ERROR] Unknown VideoHub source name: " + sourceName);
        return false;
    }
    return RouteInputToOutput(outputIndex, it->second);
}

bool VideoHubClient::RefreshInputLabels(const std::unordered_map<int, std::string>& labels) {
    std::vector<std::pair<int, std::string>> ordered(labels.begin(), labels.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const std::pair<int, std::string>& lhs,
                 const std::pair<int, std::string>& rhs) { return lhs.first < rhs.first; });

    std::ostringstream command;
    command << kInputLabelsHeader << "\n";
    for (const auto& [index, label] : ordered) {
        command << index << " " << label << "\n";
    }
    command << "\n";

    return SendCommand(command.str());
}

void VideoHubClient::LogSocketError(const std::string& context) {
    int code = WSAGetLastError();
    Logger::Error("[TECH ERROR] VideoHub socket error during " + context +
                  ": code " + std::to_string(code));
}

bool VideoHubClient::ConsumeInitialState() {
    // Set socket to non-blocking mode to read available data
    u_long mode = 1;  // 1 = non-blocking
    if (ioctlsocket(m_socket, FIONBIO, &mode) != 0) {
        LogSocketError("setting non-blocking mode");
        return false;
    }

    // Read and discard initial state data sent by VideoHub
    // The device sends PROTOCOL PREAMBLE, VERSION, VIDEO OUTPUT LOCKS, VIDEO OUTPUT ROUTING, etc.
    char buffer[4096];
    int totalBytesRead = 0;
    // Max attempts: 10 attempts * 10ms = 100ms additional wait after initial sleep
    constexpr int kMaxReadAttempts = 10;
    int attempts = 0;
    bool loggedPreview = false;

    // Wait briefly for data to arrive, then consume it
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    while (attempts < kMaxReadAttempts) {
        int bytesRead = recv(m_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytesRead > 0) {
            totalBytesRead += bytesRead;
            buffer[bytesRead] = '\0';
            // Log first chunk for debugging
            if (!loggedPreview) {
                std::string preview(buffer, std::min(bytesRead, 200));
                Logger::Info("VideoHub initial state preview: " + preview + "...");
                loggedPreview = true;
            }
            // Continue reading while data is available
        } else if (bytesRead == 0) {
            // Connection closed
            Logger::Warning("VideoHub closed connection during initial state read");
            break;
        } else {
            // WSAEWOULDBLOCK means no more data available
            int error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) {
                attempts++;
                if (totalBytesRead > 0) {
                    // We got some data, that's good enough
                    break;
                }
                // Wait a bit and try again
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                LogSocketError("reading initial state");
                break;
            }
        }
    }

    // Restore socket to blocking mode for normal operation
    mode = 0;  // 0 = blocking
    if (ioctlsocket(m_socket, FIONBIO, &mode) != 0) {
        LogSocketError("restoring blocking mode");
        return false;
    }

    Logger::Info("VideoHub initial state consumed (" + std::to_string(totalBytesRead) + " bytes)");
    return totalBytesRead > 0;
}
