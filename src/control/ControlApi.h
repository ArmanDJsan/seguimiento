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
 *   POST /arm     {"race_id":"..."}  ARMADO AUTOMATICO COMPLETO, en un solo
 *                 pedido y sin intervencion humana:
 *                   1) lock META         -> DOS camaras, criterio AND: se
 *                                           encuadra cada PTZ en su preset, se
 *                                           espera el asentamiento, se miden
 *                                           las dos y cada una tiene que dar 10
 *                                           (si no, 409 y NO manda el reload:
 *                                           no se puede verificar arriba lo que
 *                                           nunca estuvo abajo)
 *                   2) ESP32 "reload"    -> suben a la salida
 *                   3) espera N ms       (control_api.arm_reload_wait_ms)
 *                   4) lock ARRIBA       -> punto 1 del guion, DOS camaras y el
 *                                           mismo AND
 *                 Si las etapas pasan, deja el sistema armed; cualquier falla
 *                 responde 409 con "etapa", "camera" y el detalle "camaras",
 *                 y deja state=idle (nunca se arma a medias).
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

#include "ArmLock.h"

namespace Choreography {
    class ChoreographyEngine;
}

namespace Measure {
    class RankingMeasure;
}

namespace PTZ {
    class PTZController;
}

class TrackPhysicalController;

/**
 * Configuracion del bloque "control_api" de config.json.
 */
struct ControlApiConfig {
    bool enabled = false;
    int port = 8090;
    std::string apiKey;          // clave obligatoria en X-Api-Key (vacia = API no arranca)

    // ---------------------------------------------------------------------
    // Armado automatico: META -> reload -> espera -> ARRIBA
    // ---------------------------------------------------------------------

    // Camara de la META (donde estan las esferas ANTES del reload), 1-indexed
    // como el guion. 0 = no declarada: /arm responde 409 con ese motivo en vez
    // de medir una camara inventada. Produccion: 4 = PTZ_FINISH (la llegada),
    // que es donde quedan las 10 esferas al terminar la carrera anterior.
    int armMetaCamera = 4;
    // Preset PTZ para ver la META de cerca. 0 = no mover la camara (se mide la
    // vista en la que este). Produccion: 2 = closeup de la llegada, el mismo
    // que usa inicio_carrera_tlms.json antes de medir el final.
    int armMetaPreset = 2;
    int armMetaBurstMs = 0;      // 0 = usar ranking_measure.burst_ms
    int armMetaMaxMs = 0;        // 0 = usar ranking_measure.max_ms

    // Segunda camara del lock META. El lock exige las DOS (AND): cada una se
    // encuadra en su preset, se espera el asentamiento y cada una tiene que
    // medir 10. 0 = no declarada -> /arm responde 409 en la etapa meta con el
    // nombre de esta clave: una segunda camara inventada daria un 10 (o un
    // fallo) que no significa nada. Tiene que ser OTRA camara que la primaria.
    int armMetaCamera2 = 0;
    int armMetaPreset2 = 0;      // 0 = no mover la segunda camara

    // Camara primaria de ARRIBA (salida) SOLO para cuando el guion no tiene
    // MeasurePoint del punto 1: 0 = armCamera. Si el guion lo tiene, manda el
    // guion (es la camara que va a medir la carrera) y este valor se ignora con
    // un Warning: verificar otra camara daria un 10 que la carrera no respalda.
    int armArribaCamera = 0;

    // Preset PTZ de la salida (ARRIBA) antes de medir el punto 1. 0 = no mover.
    // Produccion: 1 = vista de la salida (el preset que deja el guion al
    // empezar).
    int armArribaPreset = 1;

    // Segunda camara del lock ARRIBA: mismo criterio AND que la META. 0 = no
    // declarada -> /arm responde 409 en la etapa arriba.
    int armArribaCamera2 = 0;
    int armArribaPreset2 = 0;    // 0 = no mover la segunda camara

    // Cuanto tarda el ascensor en subir las 10 esferas despues del reload.
    // En el guion: reload en t=9,0 s y medicion del punto 1 en t=15,0 s -> 6 s.
    int armReloadWaitMs = 6000;

    // Asentamiento del PTZ despues de llamar un preset (el motor tarda en
    // llegar y en enfocar; medir en movimiento da cajas borrosas).
    int armPtzSettleMs = 2000;

    // Chequeo de ARRIBA. El guion manda: si el guion cargado tiene un
    // MeasurePoint para el punto 1 se usan SU camara y SU ventana, para
    // verificar exactamente lo que va a medir la carrera. Estos valores son el
    // fallback cuando el guion no tiene ese punto.
    int armCamera = 1;           // 1-indexed, como el guion
    int armBurstMs = 0;          // 0 = usar ranking_measure.burst_ms
    int armMaxMs = 0;            // 0 = usar ranking_measure.max_ms
};

class ControlApi {
public:
    enum class State {
        Idle,       // sin carrera armada
        Armed,      // /arm ok: 10 esferas en la META y 10 ARRIBA (armado completo)
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
     * @param track      controlador del ESP32 (null: /arm no puede mandar el
     *                   reload y responde 409 en esa etapa)
     * @param ptz        controlador PTZ (null: /arm mide la vista actual)
     * @return true si el puerto quedo escuchando
     */
    bool Start(const ControlApiConfig& config,
               Choreography::ChoreographyEngine* engine,
               Measure::RankingMeasure* measure,
               const std::string& scriptPath,
               TrackPhysicalController* track = nullptr,
               PTZ::PTZController* ptz = nullptr);

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

    /**
     * Corre un lock del armado (META o ARRIBA) con DOS camaras y criterio AND:
     * encuadra cada PTZ en su preset (con asentamiento), mide las dos y exige
     * el mismo conteo en ambas. El orden y el veredicto viven en ArmLock.h
     * (probados en tests/test_arm_lock.cpp); aca solo se conectan el PTZ, el
     * medidor y la cancelacion por /abort.
     * @param etapa    "META" o "ARRIBA" (para logs y motivos)
     * @param cameras  las dos camaras del lock, con su preset
     * @param verdict  [out] veredicto con el detalle por camara
     */
    void RunArmLock(const char* etapa,
                    const std::vector<ArmLock::CameraSpec>& cameras,
                    int burstMs, int maxMs,
                    ArmLock::Verdict& verdict);

    /**
     * Arma el fragmento JSON "camaras":[{...}] con lo que midio cada camara
     * del lock. Se usa igual en el 200 y en el 409: el que lee la respuesta ve
     * los dos conteos, no solo el que fallo.
     */
    static std::string ArmCamerasJson(const std::vector<ArmLock::CameraReading>& readings);

    /**
     * Llama un preset PTZ antes de medir una etapa del armado. Sin controlador
     * PTZ o con preset<=0 no hace nada (se mide la vista actual) y lo deja
     * dicho en el log: no se finge un encuadre que no se pidio.
     * @return false solo si el preset se pidio y el VISCA lo rechazo
     */
    bool MoveArmCamera(int camera1Based, int preset, const char* etapa,
                       std::string& motivo);

    /**
     * Espera del ascensor, cortable: duerme en tramos de 100 ms para que
     * /abort y /shutdown no queden colgados 6 s detras de un /arm.
     * @return false si la cancelo un /abort o un /shutdown
     */
    bool WaitArmDelay(int ms, std::string& motivo);

    /**
     * Responde 409 de una etapa del armado y deja el sistema en idle.
     * @param spheres     esferas de la camara que hizo fallar la etapa (0 si no
     *                    se llego a medir)
     * @param camera      camara que hizo fallar la etapa (0 = etapa sin camara:
     *                    reload, espera, configuracion general)
     * @param camerasJson fragmento ya armado con ArmCamerasJson (vacio = etapa
     *                    sin mediciones)
     */
    void ArmFail(int& status, std::string& out, const char* etapa,
                 int spheres, const std::string& motivo,
                 int camera = 0, const std::string& camerasJson = std::string());

    ControlApiConfig m_config;
    Choreography::ChoreographyEngine* m_engine = nullptr;
    Measure::RankingMeasure* m_measure = nullptr;
    TrackPhysicalController* m_track = nullptr;
    PTZ::PTZController* m_ptz = nullptr;
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
    // /abort durante un /arm: la espera del ascensor y las etapas que faltan
    // se cortan en vez de seguir armando una carrera que ya nadie quiere.
    std::atomic<bool> m_armCancelled{false};
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
