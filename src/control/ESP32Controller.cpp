/**
 * ESP32Controller.cpp
 *
 * Sends HTTP GET requests to an ESP32 over WinHTTP.
 */

#include "ESP32Controller.h"
#include "../utils/Logger.h"
#include <sstream>

#pragma comment(lib, "Winhttp.lib")

namespace {
constexpr wchar_t kUserAgent[] = L"Seguimiento-ESP32/1.0";
} // namespace

ESP32Controller::ESP32Controller(const std::wstring& host, uint16_t port)
    : m_host(host), m_port(port)
{
    // Built-in endpoint mapping
    m_endpoints["iniciar"]   = L"/iniciar";
    m_endpoints["finalizar"] = L"/cerrar";
    m_endpoints["reload"]    = L"/recargar";
    m_endpoints["test"]      = L"/test";
}

ESP32Controller::~ESP32Controller() {
    Cleanup();
}

void ESP32Controller::RegisterEndpoint(const std::string& commandName,
                                       const std::wstring& path)
{
    m_endpoints[commandName] = path;
}

bool ESP32Controller::SendCommand(const std::string& commandName) {
    auto it = m_endpoints.find(commandName);
    if (it == m_endpoints.end()) {
        Logger::Warning("ESP32Controller: Unknown command '" + commandName +
                        "', attempting as literal path /" + commandName);
        std::wstring path = L"/" + Utf8ToWide(commandName);
        return GetRequest(path);
    }
    return GetRequest(it->second);
}

bool ESP32Controller::EnsureConnection() {
    if (!m_httpSession) {
        m_httpSession = WinHttpOpen(kUserAgent,
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS,
                                    0);
        if (!m_httpSession) {
            Logger::Error("ESP32Controller: Failed to open WinHTTP session");
            return false;
        }
        // Short timeout (3 s) – ESP32 should respond quickly
        DWORD timeoutMs = 3000;
        WinHttpSetOption(m_httpSession, WINHTTP_OPTION_CONNECT_TIMEOUT,
                         &timeoutMs, sizeof(timeoutMs));
        WinHttpSetOption(m_httpSession, WINHTTP_OPTION_RECEIVE_TIMEOUT,
                         &timeoutMs, sizeof(timeoutMs));
        WinHttpSetOption(m_httpSession, WINHTTP_OPTION_SEND_TIMEOUT,
                         &timeoutMs, sizeof(timeoutMs));
    }
    if (!m_httpConnection) {
        m_httpConnection = WinHttpConnect(m_httpSession, m_host.c_str(),
                                          m_port, 0);
        if (!m_httpConnection) {
            Logger::Error("ESP32Controller: Failed to connect to ESP32 at " +
                          std::string(m_host.begin(), m_host.end()) + ":" +
                          std::to_string(m_port));
            return false;
        }
    }
    return true;
}

bool ESP32Controller::GetRequest(const std::wstring& path) {
    if (!EnsureConnection()) {
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(m_httpConnection,
                                            L"GET",
                                            path.c_str(),
                                            nullptr,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            0);
    if (!hRequest) {
        Logger::Error("ESP32Controller: Failed to create request for path " +
                      std::string(path.begin(), path.end()));
        return false;
    }

    bool ok = false;

    if (!WinHttpSendRequest(hRequest,
                            WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        Logger::Error("ESP32Controller: Failed to send request to path " +
                      std::string(path.begin(), path.end()));
    } else if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        Logger::Error("ESP32Controller: Failed to receive response for path " +
                      std::string(path.begin(), path.end()));
    } else {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);
        ok = (statusCode >= 200 && statusCode < 300);
        if (!ok) {
            Logger::Warning("ESP32Controller: Received HTTP " +
                            std::to_string(statusCode) + " from ESP32");
        }
    }

    WinHttpCloseHandle(hRequest);

    // Reset connection on any failure so next call reconnects
    if (!ok) {
        if (m_httpConnection) {
            WinHttpCloseHandle(m_httpConnection);
            m_httpConnection = nullptr;
        }
    }

    return ok;
}

void ESP32Controller::Cleanup() {
    if (m_httpConnection) {
        WinHttpCloseHandle(m_httpConnection);
        m_httpConnection = nullptr;
    }
    if (m_httpSession) {
        WinHttpCloseHandle(m_httpSession);
        m_httpSession = nullptr;
    }
}

std::wstring ESP32Controller::Utf8ToWide(const std::string& str) {
    if (str.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, str.data(),
                                     static_cast<int>(str.size()),
                                     nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
                        wide.data(), needed);
    return wide;
}
