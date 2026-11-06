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
 *                     "point":N,"spheres":N,"ranking":[...],"t0":<epoch_ms>,
 *                     "engine":"...","engine_error":"..."}
 *   POST /arm     {"race_id":"..."}  "¿LA PISTA ESTA LISTA PARA UNA CARRERA?"
 *                 Verifica, sin mover la pista:
 *                   1) pista          -> ESP32 (/status), VideoHub (barrido
 *                                       Streaming 1-12 y Seguimiento 13-16),
 *                                       vMix (inputs sanos) y DeckLink (los
 *                                       canales de captura entran cuadros)
 *                   2) lock META      -> esta es la LLEGADA, donde estan las
 *                                       esferas antes de subir: DOS camaras con
 *                                       criterio AND, cada una encuadrada en su
 *                                       preset y con 10 esferas
 *                 NO manda el reload del ascensor, NO espera la subida y NO
 *                 toca el PTZ de la salida: todo eso es parte del guion, que
 *                 arranca con /start.
 *                 Si algo falla responde 409 con "etapa" (pista|meta), el
 *                 detalle y, en la etapa pista, "chequeos":[{id,ok,detalle}].
 *                 Deja state=armed solo si paso TODO (nunca se arma a medias).
 *   POST /start   dispara el guion (equivale a F12) si esta armed. NO manda
 *                 nada a vMix: el streaming lo arranca la propia coreografia,
 *                 igual que el reload, la espera y el lock de arriba. Si la
 *                 salida no tiene las 10, la compuerta NO abre y el guion se
 *                 detiene.
 *   POST /abort   detiene el guion y vuelve a idle. No toca vMix (el streaming
 *                 es del guion, no de la API).
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
#include "PistaReadiness.h"

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
    // /arm = "¿la pista esta lista para una carrera?"
    //   chequeo de pista (ESP32, VideoHub, vMix, DeckLink) + lock META
    // NO manda el reload, NO espera al ascensor y NO toca la salida: el
    // ascensor, la espera y el lock de arriba son parte del GUION (/start).
    // ---------------------------------------------------------------------

    // Camara de la META (donde estan las esferas ANTES de subir: la llegada),
    // 1-indexed como el guion. 0 = no declarada: /arm responde 409 con ese
    // motivo en vez de medir una camara inventada.
    int armMetaCamera = 4;
    // Preset PTZ para ver la META de cerca. 0 = no mover la camara (se mide la
    // vista en la que este).
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

    // Chequeo de pista antes del lock: ESP32 (/status), barrido del VideoHub
    // (Streaming 1-12 y Seguimiento 13-16), vMix (inputs sanos) y DeckLink
    // (los canales de captura entran cuadros). Es el mismo chequeo de la
    // Fase 1 de la ignicion, pero bajo pedido y con veredicto por item.
    // false = solo el lock de esferas (para pruebas de banco).
    bool armCheckPista = true;
    // false = no rutear los 16 puertos (solo verifica que el VideoHub este
    // conectado). Util si el ruteo del monitoreo molesta durante una prueba.
    bool armSweepVideoHub = true;

    // Asentamiento del PTZ despues de llamar un preset (el motor tarda en
    // llegar y en enfocar; medir en movimiento da cajas borrosas).
    int armPtzSettleMs = 2000;

    // ---------------------------------------------------------------------
    // /start = solo el guion
    // ---------------------------------------------------------------------
    // No hay flags de streaming: /start NO manda StartStreaming a vMix. El
    // streaming lo arranca la coreografia por su cuenta (y /abort no lo corta
    // porque no es de la API). Si hiciera falta un switch, va en el guion.
};

class ControlApi {
public:
    enum class State {
        Idle,       // sin carrera armada
        Armed,      // /arm ok: la pista esta lista y hay 10 esferas abajo (META)
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
     * @param track      controlador del ESP32 (null: el chequeo de pista falla
     *                   y el guion no puede mover la pista)
     * @param ptz        controlador PTZ (null: /arm mide la vista actual)
     * @param pista      handles del pipeline para el chequeo de pista del /arm
     *                   (VideoHub, DeckLink, vMix y canales de captura)
     * @return true si el puerto quedo escuchando
     */
    bool Start(const ControlApiConfig& config,
               Choreography::ChoreographyEngine* engine,
               Measure::RankingMeasure* measure,
               const std::string& scriptPath,
               TrackPhysicalController* track = nullptr,
               PTZ::PTZController* ptz = nullptr,
               const Pista::Fuente& pista = Pista::Fuente());

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

    /**
     * Foto del estado para el panel de consola del menu [12] SERVICIO API.
     *
     * Junta en una sola llamada lo mismo que responde GET /status (estado,
     * race_id, punto, esferas, motor) y lo ultimo que hizo el que manda
     * (backend u operador), para que la consola muestre un instante coherente
     * y no una mezcla de dos lecturas.
     */
    struct Snapshot {
        State state = State::Idle;
        std::string stateName = "idle";   // "idle" | "armed" | "running" | "finished"
        std::string engine = "n/d";       // estado del ChoreographyEngine
        std::string raceId;
        int point = 0;                    // ultimo punto medido (0 = ninguno)
        int spheres = 0;                  // esferas del ultimo ranking publicado
        int64_t t0 = 0;                   // epoch ms del /start (0 = sin carrera)
        bool listening = false;           // el listener HTTP esta arriba
        bool armInFlight = false;         // hay un /arm corriendo (tarda ~27 s)
        // Ultimo comando que MUEVE el sistema (/arm, /start, /abort, /shutdown).
        // /status no se registra: lo consulta el backend cada segundo y taparia
        // al comando real.
        std::string lastCommand;          // "arm", "start", ... (vacio = ninguno)
        int lastStatus = 0;               // codigo HTTP de esa respuesta
        int64_t lastAtMs = 0;             // epoch ms de esa respuesta
    };
    Snapshot GetSnapshot();

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
     * Corre un lock del armado con DOS camaras y criterio AND:
     * encuadra cada PTZ en su preset (con asentamiento), mide las dos y exige
     * el mismo conteo en ambas. El orden y el veredicto viven en ArmLock.h
     * (probados en tests/test_arm_lock.cpp); aca solo se conectan el PTZ, el
     * medidor y la cancelacion por /abort.
     * @param etapa    "META" (la llegada, abajo: unico lock del /arm)
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
     * Espera cortable (asentamiento del PTZ). Duerme en tramos de 100 ms para
     * que /abort y /shutdown no queden colgados detras de un /arm.
     * @return false si la cancelo un /abort o un /shutdown
     */
    bool WaitArmDelay(int ms, std::string& motivo);

    /**
     * Responde 409 de una etapa del armado y deja el sistema en idle.
     * @param spheres     esferas de la camara que hizo fallar la etapa (0 si no
     *                    se llego a medir)
     * @param camera      camara que hizo fallar la etapa (0 = etapa sin camara:
     *                    pista, configuracion general)
     * @param camerasJson fragmento ya armado con ArmCamerasJson (vacio = etapa
     *                    sin mediciones)
     * @param chequeosJson fragmento "chequeos":[...] del chequeo de pista
     *                    (vacio = etapa sin chequeos)
     */
    void ArmFail(int& status, std::string& out, const char* etapa,
                 int spheres, const std::string& motivo,
                 int camera = 0, const std::string& camerasJson = std::string(),
                 const std::string& chequeosJson = std::string());

    /**
     * Guarda el ultimo comando que movio el sistema, para el panel de consola.
     * Se llama desde el logger del servidor (hilo HTTP), asi que usa su propio
     * mutex y no toca m_stateMutex: no puede bloquear a un /arm en curso.
     */
    void RecordCommand(const std::string& path, int httpStatus);

    ControlApiConfig m_config;
    Choreography::ChoreographyEngine* m_engine = nullptr;
    Measure::RankingMeasure* m_measure = nullptr;
    TrackPhysicalController* m_track = nullptr;
    PTZ::PTZController* m_ptz = nullptr;
    std::string m_scriptPath;
    // Handles del pipeline para el chequeo de pista del /arm. Solo se leen
    // (ruteo del VideoHub, contadores de la captura, API de vMix y ESP32).
    Pista::Fuente m_pista;

    std::unique_ptr<Impl> m_impl;

    mutable std::mutex m_stateMutex;
    State m_state = State::Idle;
    std::string m_raceId;
    int64_t m_t0 = 0;              // epoch ms del /start
    int64_t m_startedAt = 0;       // epoch ms del /start (para el margen de "finished")

    std::atomic<bool> m_listening{false};
    std::atomic<bool> m_shutdownRequested{false};
    std::atomic<bool> m_armInFlight{false};

    // Ultimo comando que movio el sistema (panel de consola del menu [12]).
    mutable std::mutex m_lastCmdMutex;
    std::string m_lastCmdPath;     // sin la barra: "arm", "start", ...
    int m_lastCmdStatus = 0;
    int64_t m_lastCmdAt = 0;
    // /abort durante un /arm: las etapas que faltan se cortan en vez de seguir
    // armando una carrera que ya nadie quiere.
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
