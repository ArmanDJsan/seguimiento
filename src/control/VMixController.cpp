/**
 * VMixController.cpp
 *
 * Implements vMix diagnostics via HTTP (port 8088) and
 * prepares TCP control channel (port 8099) for low-latency commands.
 */

#include "VMixController.h"
#include "../utils/Logger.h"
#include <sstream>
#include <vector>
#include <ws2tcpip.h>
#include <regex>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Winhttp.lib")

namespace {
constexpr wchar_t kUserAgent[] = L"Seguimiento-VMix/1.0";

struct HttpHandle {
    explicit HttpHandle(HINTERNET h = nullptr) : handle(h) {}
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    HttpHandle(HttpHandle&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    HttpHandle& operator=(HttpHandle&& other) noexcept {
        if (this != &other) {
            if (handle) {
                WinHttpCloseHandle(handle);
            }
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    ~HttpHandle() {
        if (handle) {
            WinHttpCloseHandle(handle);
        }
    }
    HINTERNET handle;
};

bool HasOfflineInput(const std::string& xml) {
    // vMix marks inputs with state="Offline" or state="Error"
    std::regex pattern(R"(<input[^>]*state\s*=\s*\"(Offline|Error)\")", std::regex::icase);
    return std::regex_search(xml, pattern);
}
} // namespace

VMixController::VMixController(const std::wstring& host, uint16_t httpPort, uint16_t tcpPort)
    : m_host(host), m_httpPort(httpPort), m_tcpPort(tcpPort) {}

VMixController::~VMixController() {
    DisconnectTcp();
    CleanupHttp();
    if (m_winsockInitialized) {
        WSACleanup();
        m_winsockInitialized = false;
    }
}

bool VMixController::EnsureHttpConnection() {
    if (!m_httpSession) {
        m_httpSession = WinHttpOpen(kUserAgent,
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
        if (!m_httpSession) {
            Logger::Error("[TECH ERROR] Unable to open WinHTTP session for vMix");
            return false;
        }
    }
    if (!m_httpConnection) {
        m_httpConnection = WinHttpConnect(m_httpSession, m_host.c_str(), m_httpPort, 0);
        if (!m_httpConnection) {
            Logger::Error("[TECH ERROR] Unable to connect to vMix HTTP endpoint");
            return false;
        }
    }
    return true;
}

void VMixController::CleanupHttp() {
    if (m_httpConnection) {
        WinHttpCloseHandle(m_httpConnection);
        m_httpConnection = nullptr;
    }
    if (m_httpSession) {
        WinHttpCloseHandle(m_httpSession);
        m_httpSession = nullptr;
    }
}

std::string VMixController::WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) {
        return {};
    }
    int required = WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
                                       static_cast<int>(wstr.size()),
                                       nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string utf8;
    utf8.resize(static_cast<size_t>(required));
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(),
                        static_cast<int>(wstr.size()),
                        utf8.data(), required, nullptr, nullptr);
    return utf8;
}

std::string VMixController::FetchApiXml() {
    if (!EnsureHttpConnection()) {
        return {};
    }

    HttpHandle request(WinHttpOpenRequest(m_httpConnection,
                                          L"GET",
                                          L"/api",
                                          nullptr,
                                          WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          0));
    if (!request.handle) {
        Logger::Error("[TECH ERROR] Failed to create vMix HTTP request");
        return {};
    }

    if (!WinHttpSendRequest(request.handle,
                            WINHTTP_NO_ADDITIONAL_HEADERS,
                            0,
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            0,
                            0)) {
        Logger::Error("[TECH ERROR] Failed to send vMix HTTP request");
        return {};
    }

    if (!WinHttpReceiveResponse(request.handle, nullptr)) {
        Logger::Error("[TECH ERROR] Failed to receive vMix HTTP response");
        return {};
    }

    std::stringstream response;
    DWORD bytesAvailable = 0;
    do {
        if (!WinHttpQueryDataAvailable(request.handle, &bytesAvailable)) {
            Logger::Error("[TECH ERROR] Failed to query vMix response availability");
            return {};
        }
        if (bytesAvailable == 0) {
            break;
        }

        std::vector<char> buffer(bytesAvailable + 1, 0);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request.handle, buffer.data(), bytesAvailable, &bytesRead)) {
            Logger::Error("[TECH ERROR] Failed to read vMix response data");
            return {};
        }
        buffer[bytesRead] = '\0';
        response << buffer.data();
    } while (bytesAvailable > 0);

    return response.str();
}

bool VMixController::CheckInputsHealthy() {
    const std::string xml = FetchApiXml();
    if (xml.empty()) {
        Logger::Error("[HW/SW ERROR] vMix API not responding");
        return false;
    }

    if (xml.find("<inputs") == std::string::npos) {
        Logger::Error("[HW/SW ERROR] vMix API returned unexpected payload");
        return false;
    }

    if (HasOfflineInput(xml)) {
        Logger::Error("[HW/SW ERROR] vMix reports offline/error inputs");
        return false;
    }

    Logger::Info("vMix inputs healthy");
    return true;
}

void VMixController::EnsureWinsock() {
    if (m_winsockInitialized) {
        return;
    }
    WSADATA wsaData{};
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        Logger::Error("[TECH ERROR] WinSock initialization failed for vMix TCP, code: " + std::to_string(result));
        return;
    }
    m_winsockInitialized = true;
}

bool VMixController::ConnectTcp() {
    EnsureWinsock();
    if (!m_winsockInitialized) {
        return false;
    }
    if (m_tcpSocket != INVALID_SOCKET) {
        return true;
    }

    m_tcpSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_tcpSocket == INVALID_SOCKET) {
        Logger::Error("[TECH ERROR] Failed to create vMix TCP socket");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_tcpPort);
    if (InetPtonW(AF_INET, m_host.c_str(), &addr.sin_addr) != 1) {
        Logger::Error("[TECH ERROR] Invalid vMix TCP address");
        closesocket(m_tcpSocket);
        m_tcpSocket = INVALID_SOCKET;
        return false;
    }

    if (connect(m_tcpSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        Logger::Error("[TECH ERROR] Failed to connect to vMix TCP endpoint");
        closesocket(m_tcpSocket);
        m_tcpSocket = INVALID_SOCKET;
        return false;
    }

    Logger::Info("vMix TCP channel ready");
    return true;
}

void VMixController::DisconnectTcp() {
    if (m_tcpSocket != INVALID_SOCKET) {
        closesocket(m_tcpSocket);
        m_tcpSocket = INVALID_SOCKET;
        Logger::Info("vMix TCP channel closed");
    }
}

bool VMixController::IsTcpConnected() const {
    return m_tcpSocket != INVALID_SOCKET;
}

bool VMixController::SendTcpCommand(const std::string& command) {
    // Attempt reconnection if socket is not connected
    if (!IsTcpConnected()) {
        Logger::Warning("vMix TCP not connected, attempting reconnect...");
        if (!ConnectTcp()) {
            Logger::Error("[TECH ERROR] vMix TCP reconnect failed, cannot send command");
            return false;
        }
    }
    const char* buffer = command.c_str();
    int total = static_cast<int>(command.size());
    int sentTotal = 0;
    while (sentTotal < total) {
        int sent = send(m_tcpSocket, buffer + sentTotal, total - sentTotal, 0);
        if (sent == SOCKET_ERROR) {
            Logger::Error("[TECH ERROR] Failed to send vMix TCP command, closing socket");
            closesocket(m_tcpSocket);
            m_tcpSocket = INVALID_SOCKET;
            return false;
        }
        sentTotal += sent;
    }
    return true;
}
