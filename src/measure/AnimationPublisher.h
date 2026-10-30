/**
 * AnimationPublisher.h
 *
 * Publicacion UDP de un punto de medicion resuelto hacia el receptor de
 * animacion (el receptor solo dibuja: no interpreta coordenadas).
 *
 * Un datagrama por punto, sin estado de conexion:
 *   {"punto":3,"t":1793300042100,"posiciones":[3,7,1,9,4,2,10,5,8,6]}
 *
 * - `posiciones` = ORDEN de las esferas (1er lugar primero), NO coordenadas.
 * - `t`          = epoch ms del FRAME que resolvio el punto (sellado en
 *                  DeckLinkCapture::OnFrameArrived), no la hora del envio.
 *
 * El identificador de cada esfera es provisorio (orden por x); el ID real por
 * carril llega despues con la barrera IR.
 *
 * Thread safety: PublishMeasurePoint es seguro desde cualquier hilo (mutex
 * propio del socket). El llamador normal es el hilo medidor.
 */

#pragma once

#include <winsock2.h>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

/**
 * Configuracion de la salida de animacion (bloque "animation" de config.json)
 */
struct AnimationPublisherConfig {
    bool enabled = false;
    std::string host = "127.0.0.1";
    uint16_t port = 0;
};

class AnimationPublisher {
public:
    explicit AnimationPublisher(const AnimationPublisherConfig& config = AnimationPublisherConfig());
    ~AnimationPublisher();

    // Non-copyable
    AnimationPublisher(const AnimationPublisher&) = delete;
    AnimationPublisher& operator=(const AnimationPublisher&) = delete;

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
     * @param punto        Numero de punto del guion (1..4 en fase 1)
     * @param frameEpochMs Instante del frame que resolvio el punto (epoch ms)
     * @param posiciones   Orden de las esferas (1er lugar primero)
     * @return true si el datagrama se envio completo
     */
    bool PublishMeasurePoint(int punto, int64_t frameEpochMs, const std::vector<int>& posiciones);

    bool IsEnabled() const { return m_config.enabled; }
    std::string GetLastError() const;

    uint64_t GetPublishCount() const { return m_publishCount.load(); }
    uint64_t GetFailCount() const { return m_failCount.load(); }

    /**
     * Payload JSON exacto que se manda por UDP (expuesto para tests/logs)
     */
    static std::string BuildPayload(int punto, int64_t frameEpochMs,
                                    const std::vector<int>& posiciones);

private:
    void SetLastError(const std::string& error);

    AnimationPublisherConfig m_config;
    std::atomic<bool> m_initialized;
    mutable std::mutex m_mutex;
    std::string m_lastError;
    std::atomic<uint64_t> m_publishCount;
    std::atomic<uint64_t> m_failCount;

    SOCKET m_socket;
    sockaddr_in m_target;
    bool m_winsockInitialized;
};
