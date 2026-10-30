/**
 * ArrivalEmitter.cpp
 *
 * Implementación del emisor del orden de llegada (stub).
 */

#include "ArrivalEmitter.h"
#include "../json.hpp"
#include "../utils/Logger.h"

using json = nlohmann::json;

ArrivalEmitter::ArrivalEmitter(const ArrivalEndpointConfig& cfg)
    : m_cfg(cfg)
{}

bool ArrivalEmitter::EmitConfirmedOrder(const std::vector<int>& order) {
    if (!m_cfg.enabled) {
        Logger::Info("[ARRIVAL] Emisión deshabilitada (endpoint_enabled=false)");
        return false;
    }
    if (m_emitted) {
        return false;  // una sola emisión por confirmación
    }

    json j;
    j["order"] = order;
    std::string payload = j.dump();

    if (Send(payload)) {
        m_emitted = true;
        return true;
    }
    return false;
}

bool ArrivalEmitter::Send(const std::string& payload) {
    // STUB: sin red. Reemplazar por HTTP POST (WinHTTP) cuando exista un endpoint real.
    std::string url = m_cfg.endpointUrl.empty() ? std::string("(sin URL)") : m_cfg.endpointUrl;
    Logger::Info("[ARRIVAL][stub] endpoint=" + url + " payload=" + payload);
    return true;
}
