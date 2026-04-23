/**
 * TrackPhysicalController.h
 *
 * HTTP client for controlling the ESP32-based physical track controller.
 * Provides simple helpers to trigger mechanical actions exposed by the device.
 */

#pragma once

// Prevent Windows.h from defining min/max macros that conflict with std::min/std::max
// Must be defined before any Windows headers
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <string>
#include <cstdint>
#include <windows.h>
#include <winhttp.h>

class TrackPhysicalController {
public:
    // Endpoint values remain in Spanish to match the ESP32 firmware API; field names stay in English for readability.
    struct Endpoints {
        std::wstring testEndpoint = L"/status";
        std::wstring openGateEndpoint = L"/iniciar";
        std::wstring openElevatorEndpoint = L"/recargar";
        std::wstring closeGatesEndpoint = L"/finalizar";
    };

    TrackPhysicalController(const std::wstring& host, uint16_t port, Endpoints endpoints = {});
    ~TrackPhysicalController();

    // Spanish API required by the ESP32 control specification
    bool ejecutarTest();
    bool abrirCompuerta();
    bool abrirAscensor();
    bool cerrarCompuertas();

    // English-friendly aliases mirroring the required Spanish API
    bool executeTest();
    bool openGate();
    bool openElevator();
    bool closeGates();

private:
    bool EnsureConnection();
    bool PerformGet(const std::wstring& path);

    std::wstring m_host;
    uint16_t m_port;
    Endpoints m_endpoints;
    HINTERNET m_session = nullptr;
    HINTERNET m_connection = nullptr;
};
