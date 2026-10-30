/**
 * ArrivalEmitter.h
 *
 * Emite el orden de llegada confirmado a un endpoint.
 * Actualmente la emisión es un STUB: solo loguea el payload JSON.
 * El método privado Send() es el punto a reemplazar por un HTTP POST real (WinHTTP)
 * cuando exista un endpoint de destino.
 *
 * Payload: {"order":[5,3,1,...]}
 */

#pragma once

#include <string>
#include <vector>

/**
 * Configuración del endpoint de emisión del orden de llegada
 */
struct ArrivalEndpointConfig {
    bool enabled = true;
    std::string endpointUrl;   // Futuro destino HTTP POST (hoy solo se loguea)
};

/**
 * ArrivalEmitter - emite el resultado confirmado (stub por ahora)
 */
class ArrivalEmitter {
public:
    explicit ArrivalEmitter(const ArrivalEndpointConfig& cfg = ArrivalEndpointConfig());

    /**
     * Construye el payload JSON y lo envía. Una sola emisión por confirmación.
     * @return true si se emitió (o se registró el stub).
     */
    bool EmitConfirmedOrder(const std::vector<int>& order);

    bool IsEnabled() const { return m_cfg.enabled; }
    bool HasEmitted() const { return m_emitted; }

    /** Re-armar para poder emitir una nueva confirmación. */
    void Reset() { m_emitted = false; }

private:
    /** STUB: loguea el payload y devuelve true. Aquí irá el HTTP POST real. */
    bool Send(const std::string& payload);

    ArrivalEndpointConfig m_cfg;
    bool m_emitted = false;
};
