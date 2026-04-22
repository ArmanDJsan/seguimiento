/**
 * VMixController.h
 *
 * Dual-path controller for vMix diagnostics (HTTP API) and
 * realtime commands (TCP socket).
 */

#pragma once

// Prevent Windows.h from defining min/max macros that conflict with std::min/std::max
// Must be defined before winsock2.h which may include windows.h
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <string>
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>

class VMixController {
public:
    VMixController(const std::wstring& host = L"127.0.0.1",
                   uint16_t httpPort = 8088,
                   uint16_t tcpPort = 8099);
    ~VMixController();

    // Diagnostics: query vMix XML and ensure inputs are online
    bool CheckInputsHealthy();

    // TCP control: prepare low-latency connection for CUT/FADE
    bool ConnectTcp();
    void DisconnectTcp();
    bool IsTcpConnected() const;
    bool SendTcpCommand(const std::string& command);

private:
    std::wstring m_host;
    uint16_t m_httpPort;
    uint16_t m_tcpPort;

    // WinHTTP handles (RAII)
    HINTERNET m_httpSession = nullptr;
    HINTERNET m_httpConnection = nullptr;

    // TCP socket
    SOCKET m_tcpSocket = INVALID_SOCKET;
    bool m_winsockInitialized = false;

    bool EnsureHttpConnection();
    std::string FetchApiXml();
    static std::string WideToUtf8(const std::wstring& wstr);
    void CleanupHttp();
    void EnsureWinsock();
};
