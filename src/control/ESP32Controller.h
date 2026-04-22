/**
 * ESP32Controller.h
 *
 * HTTP client for sending commands to an ESP32 device.
 * Sends HTTP GET requests to configurable endpoints (iniciar, cerrar, recargar).
 *
 * Usage:
 *   ESP32Controller ctrl("192.168.88.114", 80);
 *   ctrl.SendCommand("iniciar");   // → GET /iniciar
 *   ctrl.SendCommand("finalizar"); // → GET /cerrar  (maps "finalizar" to the "/cerrar" endpoint)
 *   ctrl.SendCommand("reload");    // → GET /recargar
 */

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <string>
#include <map>
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>

class ESP32Controller {
public:
    /**
     * Constructor
     * @param host  IP address of the ESP32
     * @param port  HTTP port (default 80)
     */
    explicit ESP32Controller(const std::wstring& host = L"192.168.88.114",
                             uint16_t port = 80);
    ~ESP32Controller();

    // Non-copyable
    ESP32Controller(const ESP32Controller&) = delete;
    ESP32Controller& operator=(const ESP32Controller&) = delete;

    /**
     * Register a named command with its URL path.
     * Built-in defaults: iniciar→/iniciar, finalizar→/cerrar, reload→/recargar
     */
    void RegisterEndpoint(const std::string& commandName, const std::wstring& path);

    /**
     * Send a named command to the ESP32.
     * The name is resolved to a URL path via RegisterEndpoint().
     * @param commandName  Logical command name (e.g. "iniciar", "finalizar", "reload")
     * @return true if the HTTP request succeeded (2xx response)
     */
    bool SendCommand(const std::string& commandName);

private:
    std::wstring m_host;
    uint16_t     m_port;

    HINTERNET m_httpSession    = nullptr;
    HINTERNET m_httpConnection = nullptr;

    std::map<std::string, std::wstring> m_endpoints;

    bool EnsureConnection();
    bool GetRequest(const std::wstring& path);
    void Cleanup();

    static std::wstring Utf8ToWide(const std::string& str);
};
