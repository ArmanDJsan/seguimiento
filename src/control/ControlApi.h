/**
 * ControlApi.h
 *
 * API HTTP de control del VIB para el BACKEND (modo --service).
 *
 * El VIB vive detras de la tailnet, asi que esta API NO se expone a internet:
 * escucha en 0.0.0.0:<port> y solo acepta
 *   - IPs de la tailnet (100.64.0.0/10) y loopback (127.0.0.1 / ::1)
 *   - el header X-Api-Key con la clave del bloque "control_api"
 *
 * Endpoints (JSON):
 *   GET  /status  -> {"state":"idle|armed|running|finished","race_id":"...",
 *                     "point":N,"spheres":N,"ranking":[...],"t0":<epoch_ms>}
 *   POST /arm     {"race_id":"..."}  mide el punto 1 (una sola rafaga, encolada
 *                 como todas las mediciones) y responde 200 si hay 10 esferas
 *                 o 409 si no. Deja el sistema armed.
 *   POST /start   dispara el guion (equivale a F12) si esta armed.
 *   POST /abort   detiene el guion y vuelve a idle.
 *   POST /shutdown (extra) apaga el proceso: en --service no hay teclado.
 *
 * Reglas duras:
 *  - El handler HTTP NUNCA toca la captura ni el zero-copy: para medir usa
 *    Measure::RankingMeasure::EnqueueProbe(), que encola en la misma cola del
 *    hilo medidor (sin abrir clip ni publicar por UDP).
 *  - El estado de la API es independiente del EngineState del guion: el guion
 *    puede terminar solo y la API lo refleja como "finished" (via
 *    ChoreographyEngine::IsFinished()).
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Choreography {
    class ChoreographyEngine;
}

namespace Measure {
    class RankingMeasure;
}

/**
 * Configuracion del bloque "control_api" de config.json.
 */
struct ControlApiConfig {
    bool enabled = false;
    int port = 8090;
    std::string apiKey;          // clave obligatoria en X-Api-Key (vacia = API no arranca)

    // Chequeo de /arm. El guion manda: si el guion cargado tiene un MeasurePoint
    // para el punto 1 se usan SU camara y SU ventana, para verificar exactamente
    // lo que va a medir la carrera. Estos valores son el fallback.
    int armCamera = 1;           // 1-indexed, como el guion
    int armBurstMs = 0;          // 0 = usar ranking_measure.burst_ms
    int armMaxMs = 0;            // 0 = usar ranking_measure.max_ms
};

class ControlApi {
public:
    enum class State {
        Idle,       // sin carrera armada
        Armed,      // /arm ok: punto 1 verificado con 10 esferas
        Running,    // guion disparado por /start
        Finished    // el guion termino solo
    };

    ControlApi();
    ~ControlApi();

    ControlApi(const ControlApi&) = delete;
    ControlApi& operator=(const ControlApi&) = delete;

    /**
     * Levanta el listener HTTP en su propio hilo (no bloquea).
     * @param config     bloque "control_api"
     * @param engine     motor de coreografia (puede ser null si esta deshabilitado)
     * @param measure    medidor por evento (puede ser null: /arm devuelve 409)
     * @param scriptPath guion a recargar si el motor no tiene ninguno cargado
     * @return true si el puerto quedo escuchando
     */
    bool Start(const ControlApiConfig& config,
               Choreography::ChoreographyEngine* engine,
               Measure::RankingMeasure* measure,
               const std::string& scriptPath);

    /**
     * Baja el listener y espera al hilo. Idempotente.
     * Llamar ANTES de destruir el motor/medidor: la API los referencia.
     */
    void Stop();

    bool IsRunning() const { return m_listening.load(); }

    /**
     * POST /shutdown: el bucle de --service debe terminar.
     * Se hace efectivo 500 ms despues del pedido, para que la respuesta HTTP
     * salga antes de que el bucle principal desarme el pipeline.
     */
    bool ShutdownRequested() const;

    // Estado actual (thread-safe). Refresca "running -> finished" mirando el motor.
    State GetState();
    static const char* StateName(State state);

    int Port() const { return m_config.port; }

    struct Impl;   // httplib vive en el .cpp

private:
    // Handlers. Devuelven codigo HTTP + body JSON: httplib no aparece en este
    // header, asi que el resto del proyecto no arrastra el servidor.
    void HandleStatus(int& status, std::string& body);
    void HandleArm(const std::string& requestBody, int& status, std::string& body);
    void HandleStart(int& status, std::string& body);
    void HandleAbort(int& status, std::string& body);
    void HandleShutdown(int& status, std::string& body);

    /**
     * Sonda de /arm: encola UNA medicion en la camara pedida y espera el
     * resultado del hilo medidor.
     * @param camera1Based camara 1-indexed (como el guion)
     * @param spheres      [out] esferas resueltas
     * @param motivo       [out] por que no se pudo medir (vacio si midio)
     * @return false si no hubo resultado (timeout, medidor ocupado, etc.)
     */
    bool DoArmProbe(int camera1Based, int burstMs, int maxMs,
                    int& spheres, std::string& motivo);

    ControlApiConfig m_config;
    Choreography::ChoreographyEngine* m_engine = nullptr;
    Measure::RankingMeasure* m_measure = nullptr;
    std::string m_scriptPath;

    std::unique_ptr<Impl> m_impl;

    mutable std::mutex m_stateMutex;
    State m_state = State::Idle;
    std::string m_raceId;
    int64_t m_t0 = 0;              // epoch ms del /start
    int64_t m_startedAt = 0;       // epoch ms del /start (para el margen de "finished")

    std::atomic<bool> m_listening{false};
    std::atomic<bool> m_shutdownRequested{false};
    std::atomic<bool> m_armInFlight{false};
    std::atomic<int64_t> m_shutdownAt{0};

    // Espera del resultado de la sonda de /arm (la entrega el hilo medidor).
    mutable std::mutex m_resultMutex;
    std::condition_variable m_resultCv;
    int64_t m_probeJobId = 0;
    bool m_probeDone = false;
    int m_probeSpheres = 0;
    std::string m_probeMotivo;

public:
    // Llamada por el hilo medidor a traves del callback de resultados.
    void OnProbeResult(int64_t jobId, int spheres, const std::string& motivo);
};
