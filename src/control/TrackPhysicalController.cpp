/**
 * TrackPhysicalController.cpp
 *
 * Implements basic HTTP calls to the ESP32 controller.
 * All failures are surfaced as technical errors to keep vision errors isolated.
 */

#include "TrackPhysicalController.h"
#include "../utils/Logger.h"
#include <climits>
#include <string>
#include <utility>

#pragma comment(lib, "Winhttp.lib")

namespace {
constexpr wchar_t kUserAgent[] = L"Seguimiento-Controlador/1.0"; // Spanish identifier is intentional

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) {
        return {};
    }
    if (wstr.size() > static_cast<size_t>(INT_MAX)) {
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

struct HttpHandle {
    explicit HttpHandle(HINTERNET h = nullptr) : handle(h) {}
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
    HttpHandle(HttpHandle&& other) noexcept : handle(other.handle) {
        other.handle = nullptr;
    }
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
} // namespace

TrackPhysicalController::TrackPhysicalController(const std::wstring& host,
                                                 uint16_t port,
                                                 Endpoints endpoints)
    : m_host(host),
      m_port(port),
      m_endpoints(std::move(endpoints)) {}

TrackPhysicalController::~TrackPhysicalController() {
    if (m_connection) {
        WinHttpCloseHandle(m_connection);
        m_connection = nullptr;
    }
    if (m_session) {
        WinHttpCloseHandle(m_session);
        m_session = nullptr;
    }
}

bool TrackPhysicalController::ejecutarTest() {
    return executeTest();
}

bool TrackPhysicalController::abrirCompuerta() {
    return openGate();
}

bool TrackPhysicalController::abrirAscensor() {
    return openElevator();
}

bool TrackPhysicalController::cerrarCompuertas() {
    return closeGates();
}

bool TrackPhysicalController::executeTest() {
    return PerformGet(m_endpoints.testEndpoint);
}

bool TrackPhysicalController::openGate() {
    return PerformGet(m_endpoints.openGateEndpoint);
}

bool TrackPhysicalController::openElevator() {
    return PerformGet(m_endpoints.openElevatorEndpoint);
}

bool TrackPhysicalController::closeGates() {
    return PerformGet(m_endpoints.closeGatesEndpoint);
}

bool TrackPhysicalController::EnsureConnection() {
    if (!m_session) {
        m_session = WinHttpOpen(kUserAgent,
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS,
                                0);
        if (!m_session) {
            Logger::Error("[TECH ERROR] Failed to open WinHTTP session for ESP32");
            return false;
        }
    }

    if (!m_connection) {
        m_connection = WinHttpConnect(m_session, m_host.c_str(), m_port, 0);
        if (!m_connection) {
            Logger::Error("[TECH ERROR] Failed to connect to ESP32 host");
            return false;
        }
    }

    return true;
}

bool TrackPhysicalController::PerformGet(const std::wstring& path) {
    if (!EnsureConnection()) {
        return false;
    }

    HttpHandle request(WinHttpOpenRequest(m_connection,
                                          L"GET",
                                          path.c_str(),
                                          nullptr,
                                          WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          0));
    if (!request.handle) {
        Logger::Error("[TECH ERROR] Failed to create HTTP request for " + WideToUtf8(path));
        return false;
    }

    bool result = WinHttpSendRequest(request.handle,
                                     WINHTTP_NO_ADDITIONAL_HEADERS,
                                     0,
                                     WINHTTP_NO_REQUEST_DATA,
                                     0,
                                     0,
                                     0);
    if (!result) {
        Logger::Error("[TECH ERROR] Failed to send HTTP request to " + WideToUtf8(path));
        return false;
    }

    result = WinHttpReceiveResponse(request.handle, nullptr);
    if (!result) {
        Logger::Error("[TECH ERROR] Failed to receive HTTP response from " + WideToUtf8(path));
        return false;
    }

    DWORD statusCode = 0;
    DWORD size = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request.handle,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &statusCode,
                             &size,
                             WINHTTP_NO_HEADER_INDEX)) {
        Logger::Error("[TECH ERROR] Unable to query HTTP status for " + WideToUtf8(path));
        return false;
    }

    if (statusCode != 200) {
        Logger::Error("[TECH ERROR] ESP32 returned HTTP " + std::to_string(statusCode) +
                      " for path " + WideToUtf8(path));
        return false;
    }

    Logger::Info("ESP32 action executed successfully: " + WideToUtf8(path));
    return true;
}
