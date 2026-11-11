/**
 * RankingPublisher.h
 *
 * Publicacion UDP de UN punto de medicion resuelto hacia TouchDesigner.
 *
 * El disparo es la coreografia: cada evento `MeasurePoint` encola un trabajo y
 * el hilo medidor manda **un solo datagrama** cuando resuelve ese punto.
 *
 * Payload (contrato fijo, ver RankingPayload.h):
 *   {"type":"ranking","seq":N,"canicas":[{"id":0,"puesto":3},...]}
 *
 * Reglas:
 *  - Un datagrama por punto de medicion (no hay cadencia continua).
 *  - `seq` incrementa una vez por datagrama emitido.
 *  - `id` de TouchDesigner: ballID 10 -> 0, resto igual.
 *  - El array siempre tiene 10 canicas.
 *
 * Thread safety: PublishMeasurePoint es seguro desde cualquier hilo.
 */

#pragma once

#include <winsock2.h>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>

#include "../output/RankingPayload.h"

/**
 * Configuracion de la salida UDP (bloque "ranking_publisher" de config.json)
 */
struct RankingPublisherConfig {
    bool enabled = false;
    std::string host = "127.0.0.1";
    uint16_t port = 0;
};

class RankingPublisher {
public:
    explicit RankingPublisher(const RankingPublisherConfig& config = RankingPublisherConfig());
    ~RankingPublisher();

    // Non-copyable
    RankingPublisher(const RankingPublisher&) = delete;
    RankingPublisher& operator=(const RankingPublisher&) = delete;

    /**
     * Inicializa Winsock y el socket UDP. Si enabled=false devuelve true sin
     * abrir socket (publisher inerte).
     */
    bool Initialize();

    /**
     * Cierra el socket y libera Winsock.
     */
    void Shutdown();

    /**
     * Envia UN datagrama UDP con el punto de medicion resuelto.
     * @param punto        Numero de punto del guion (1..6)
     * @param frameEpochMs Instante del frame que resolvio el punto (epoch ms, solo log)
     * @param posiciones   ballIDs en orden (1er lugar primero)
     * @return true si el datagrama se envio completo
     */
    void SetRaceId(const std::string& id) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state.SetRacePrefix(id);
    }

    /** Destino alternativo del mismo payload (el overlay HTTP local). */
    using PublishSink = std::function<void(const std::string&)>;
    void SetPublishSink(PublishSink sink) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sink = std::move(sink);
    }

    bool PublishMeasurePoint(int punto, int64_t frameEpochMs, const std::vector<int>& posiciones);

    bool IsEnabled() const { return m_config.enabled; }
    std::string GetLastError() const;

    uint64_t GetPublishCount() const { return m_publishCount.load(); }
    uint64_t GetFailCount() const { return m_failCount.load(); }
    uint32_t GetSeq() const { return m_state.seq; }

    /**
     * Payload exacto que se enviaria con el estado actual (expuesto para tests/logs)
     */
    std::string BuildCurrentPayload() const;

private:
    void SetLastError(const std::string& error);

    RankingPublisherConfig m_config;
    PublishSink m_sink;   ///< se llama con el mismo JSON que va por UDP
    std::atomic<bool> m_initialized;
    mutable std::mutex m_mutex;
    std::string m_lastError;
    std::atomic<uint64_t> m_publishCount;
    std::atomic<uint64_t> m_failCount;

    SOCKET m_socket;
    sockaddr_in m_target;
    bool m_winsockInitialized;

    ranking::RankingState m_state;
};
