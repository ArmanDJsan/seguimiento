/**
 * RankingMeasure.h
 *
 * Medidor por evento (fase 1): el motor de coreografia NO mide; en el `case`
 * del evento MeasurePoint solo encola y sigue con la linea de tiempo. Un hilo
 * medidor aparte toma frames del anillo corto, infiere, resuelve el orden de
 * las esferas y manda UN datagrama UDP por punto.
 *
 *   hilo coreografia -> EnqueueMeasure() -> sigue
 *                            |
 *                   hilo medidor: historia -> orden -> 1x sendto()
 *
 * Reglas duras (ver plan del dueno):
 *  - El hilo medidor NO llama CallPreset, NO manda comandos a vMix y NO toca
 *    estado del motor de coreografia.
 *  - Cola con tope (8) y descarte del mas viejo: si algo se atrasa se pierde
 *    una medicion, nunca se frena la coreografia.
 *  - Anillo corto por camara (kRingFrames frames UYVY en memoria pinned). NO es
 *    un buffer circular de la carrera entera: es el pre-roll para tener el
 *    frame de ANTES del disparo.
 *  - Anillo sin lock global: seqlock por slot (contador de generacion) para que
 *    una camara atrasada no frene la captura de las otras tres.
 *  - NO infiere: el pipeline ya infiere cada cuadro de las 4 camaras y se lo
 *    pasa por OnDetections. El medidor solo guarda historia y arma la evidencia.
 *  - Clip de evidencia POR CAMARA (un contexto por slot, kMaxCameras): dos
 *    mediciones solapadas en camaras distintas graban cada una su MP4.
 */

#pragma once

#include <cuda_runtime.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../ai/InferenceEngine.h"
#include "RankingPublisher.h"

namespace Measure {

/**
 * Anillo corto de frames UYVY en memoria pinned, uno por camara.
 *
 * Escritura: hilo de captura (DeckLinkCapture::ProcessFrame).
 * Lectura:   hilo medidor (copia a un buffer de device con cudaMemcpyAsync).
 *
 * Sincronizacion: seqlock por slot (atomic<uint64_t> impar=escribiendo,
 * par=estable). No hay mutex ni lock global: un lector lento de la camara A no
 * bloquea la escritura de la camara B.
 */
class FrameRing {
public:
    // Rango pedido por el dueno: 8..16 frames UYVY 1080p (4,15 MB/frame).
    // Se elige 8 = 132 MB pinned para las 4 PTZ (8 x 4,15 x 4).
    static constexpr int kSlots = 8;

    FrameRing() = default;
    ~FrameRing();

    FrameRing(const FrameRing&) = delete;
    FrameRing& operator=(const FrameRing&) = delete;

    /**
     * Reserva el anillo en memoria pinned (cudaHostAlloc).
     * @param cameraID ID de camara/canal (0-based, el mismo de VideoChannel)
     */
    bool Initialize(int cameraID, unsigned int width, unsigned int height);
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }
    int CameraID() const { return m_cameraID; }
    unsigned int Width() const { return m_width; }
    unsigned int Height() const { return m_height; }
    size_t FrameBytes() const { return m_frameBytes; }

    /**
     * Copia un frame UYVY al anillo (llamado por el hilo de captura).
     * @param src     puntero al frame UYVY (memoria pinned del DeckLink)
     * @param bytes   bytes disponibles
     * @param epochMs instante del frame sellado en OnFrameArrived
     */
    void Push(const void* src, size_t bytes, int64_t epochMs);

    /**
     * Indice del ultimo frame publicado (-1 si todavia no hay ninguno).
     */
    int64_t LatestIndex() const { return m_nextIndex.load(std::memory_order_acquire) - 1; }

    /**
     * Lee solo el timestamp de un slot (seqlock). Devuelve false si el slot
     * esta siendo escrito o el indice ya fue reciclado.
     */
    bool TryGetTimestamp(int64_t index, int64_t& epochMs) const;

    /**
     * Copia un frame del anillo a un buffer de device propio.
     * @param deviceDst destino en GPU
     * @param dstBytes  capacidad del destino
     * @param stream    stream propio del medidor
     * @param epochMs   [out] instante del frame copiado
     * @return false si el slot fue sobrescrito durante la copia (se descarta)
     */
    bool CopyToDevice(int64_t index, void* deviceDst, size_t dstBytes,
                      cudaStream_t stream, int64_t& epochMs) const;

private:
    struct Slot {
        void* data = nullptr;
        int64_t epochMs = 0;
        int64_t index = -1;             // indice del frame que hoy ocupa el slot
        std::atomic<uint64_t> seq{0};   // impar = escritura en curso
    };

    Slot m_slots[kSlots];
    std::atomic<int64_t> m_nextIndex{0};
    int m_cameraID = -1;
    unsigned int m_width = 0;
    unsigned int m_height = 0;
    size_t m_frameBytes = 0;
    bool m_initialized = false;
};

/**
 * Configuracion del medidor (bloque "ranking_measure" de config.json).
 *
 * Estos valores son los DEFAULTS: si el evento MeasurePoint del guion trae
 * burst_ms/max_ms, gana el evento.
 */
struct RankingMeasureConfig {
    int burstMs = 400;   // rafaga corta de medicion por punto
    int maxMs = 2000;    // techo si no salen las 10 esferas
    // true  = si dos esferas quedan con la misma clase no se publica (produccion).
    // false = publica igual, marcado como provisorio (solo para armar/validar).
    bool strictClasses = true;

    // Evidencia: guardar los cuadros de la rafaga en disco (UYVY crudo) y armar
    // el MP4 con ffmpeg si esta en PATH. Apagado por defecto.
    bool saveClip = false;
    std::string saveDir = "measure_clips";
    int saveFps = 30;
    int saveMaxFrames = 90;
    // Carrera COMPLETA en RAM, una por camara, para volcarla a MP4 al final.
    // Apagado por defecto: con false NO se reserva memoria ni se copia un solo
    // cuadro (el if va antes de cualquier cudaMemcpy, ver OnDetections).
    bool saveRace = false;
    // Tope de RAM por camara, en segundos a 30 fps: 50 s x 30 fps x 4,15 MB =
    // ~6,2 GB por camara. Presupuesto del dueno para las 4 camaras en el peor
    // caso: ~56 GB (la maquina tiene 128 GB, esta previsto). Cubre los
    // race_seconds de carrera (45 s) con margen y acota el anillo: el cuadro mas
    // viejo se descarta.
    int raceMaxSeconds = 50;
    // Cuanto dura la carrera de verdad, contada desde que abre la compuerta
    // (ESP32 "iniciar"). A los race_seconds se cierra la carrera y el anillo se
    // vuelca solo: despues de eso no se graba nada mas. Medido: 42 s de carrera
    // + 3 s de cola para que se vea la llegada asentarse.
    int raceSeconds = 45;
    // Ejecutable de ffmpeg. Puede ser "ffmpeg" (PATH) o una ruta completa.
    // Se admite %LOCALAPPDATA% y demas variables: las expande cmd, asi que la
    // ruta puede tener acentos sin problema de codificacion en el codigo.
    std::string ffmpegCmd = "ffmpeg";

    // Quemar las cajas del detector en los MP4 (clips por punto y carrera).
    //
    // Se dibujan AL COMPRIMIR y desde las detecciones YA GUARDADAS: se arma un
    // .ass con un rectangulo por caja y se le pasa a ffmpeg con -vf ass=... .
    // NO se toca la captura ni el zero-copy (no hay ni un pixel dibujado en el
    // camino caliente). Apagado por defecto: con false no se genera un solo
    // .ass y el comando de ffmpeg es exactamente el de siempre.
    bool drawBoxes = false;
};

/**
 * Trabajo de medicion encolado por el motor de coreografia
 */
struct MeasureJob {
    int camera = 0;                 // camara del evento (0-based)
    int punto = 0;                  // numero de punto del guion
    int burstMs = 400;              // rafaga corta
    int maxMs = 2000;               // tope si no aparecen las 10 esferas
    int64_t requestedEpochMs = 0;   // instante del disparo (epoch ms)
    int64_t jobId = 0;              // identificador para emparejar el resultado
    // Sonda (API de control, /arm): NO abre clip ni publica por UDP. Solo mide
    // y reporta por el callback de resultados. Se encola igual que todo lo
    // demas, asi que no bloquea ni toca el hilo de captura.
    bool probe = false;
};

/**
 * RankingMeasure - duenio del anillo, la historia por camara, la cola, el hilo
 * medidor, el publisher UDP y los clips de evidencia (uno por camara).
 */
class RankingMeasure {
public:
    static constexpr int kMaxCameras = 4;        // 4 PTZ radar
    static constexpr size_t kQueueCapacity = 8;  // tope de la cola de mediciones
    static constexpr int kTargetSpheres = 10;
    // Buffer propio de device para el frame a inferir. La captura es 4x 1080p
    // (1920x1080 UYVY = 4,15 MB), asi que se reserva una vez en Initialize y
    // nunca se reasigna (el hilo medidor es unico y lo usa en serie).
    static constexpr size_t kMaxFrameBytes = 1920u * 1080u * 2u;

    /**
     * Resultado de UNA medicion, para observadores externos (API de control).
     * No cambia en nada la medicion: es un aviso de lo que ya se resolvio.
     */
    struct MeasureResult {
        int64_t jobId = 0;
        int camera = 0;         // 0-based
        int punto = 0;
        int spheres = 0;        // esferas resueltas (posiciones.size())
        int maxDetections = 0;  // pico de detecciones en la rafaga
        bool published = false; // se mando el datagrama UDP del punto
        bool probe = false;     // sonda de /arm (no publica)
        std::string motivo;     // por que no se publico (vacio si publico)
        int64_t epochMs = 0;    // instante del frame que resolvio el punto
    };

    /** Se llama en el HILO MEDIDOR: el callback no debe bloquear. */
    using MeasureResultCallback = std::function<void(const MeasureResult&)>;

    RankingMeasure();
    ~RankingMeasure();

    RankingMeasure(const RankingMeasure&) = delete;
    RankingMeasure& operator=(const RankingMeasure&) = delete;

    /**
     * Crea anillos, publisher y el hilo medidor. NO crea motor de inferencia:
     * el pipeline ya infiere cada cuadro y nos lo pasa por OnDetections.
     * @param pubConfig       bloque "ranking_publisher" de config.json
     * @param measureConfig   bloque "ranking_measure" (defaults de burst/techo)
     * @param inferenceConfig misma config que usa el camino de las PTZ
     */
    bool Initialize(const RankingPublisherConfig& pubConfig,
                    const RankingMeasureConfig& measureConfig,
                    const InferenceEngineConfig& inferenceConfig);

    /**
     * Apaga el hilo medidor (join), el publisher y libera anillos/GPU.
     */
    void Shutdown();

    /**
     * Encola una medicion. NO bloquea: si la cola esta llena se descarta la
     * mas vieja. Esto es lo unico que llama el `case` del motor.
     * @param probe true = sonda de /arm: no abre clip ni publica por UDP
     * @return jobId para emparejar el resultado (0 si se rechazo)
     */
    int64_t Enqueue(int camera, int punto, int burstMs, int maxMs, bool probe = false);

    /**
     * Sonda de la API de control (/arm): mide una camara sin publicar nada.
     * @param burstMs 0 = default de ranking_measure
     * @param maxMs   0 = default de ranking_measure
     * @return jobId para emparejar el resultado (0 si se rechazo)
     */
    int64_t EnqueueProbe(int camera, int burstMs, int maxMs);

    /**
     * Observador de resultados (API de control). Se guarda/protege con mutex;
     * nullptr lo desengancha.
     */
    void SetResultCallback(MeasureResultCallback callback);

    /**
     * Copia thread-safe del ULTIMO ranking publicado (ballIDs en orden de
     * llegada). El "final" es el ultimo punto publicado de la carrera: la
     * agregacion entre puntos no es parte de esto.
     * @return false si todavia no se publico ningun punto
     */
    bool GetLastRanking(std::vector<int>& posiciones, int& punto, int64_t& epochMs) const;

    /** Borra el ranking recordado (arranque de una carrera nueva). */
    void ClearLastRanking();

    /**
     * Crea (idempotente) el anillo de una camara y el buffer de device propio
     * del medidor. Se llama desde el enganche de captura, cuando ya se conocen
     * las dimensiones reales del canal.
     * @return anillo de esa camara (nullptr si la camara esta fuera de rango)
     */
    FrameRing* SetupCamera(int cameraID, unsigned int width, unsigned int height);


    /**
     * El pipeline YA infiere en todas las camaras todo el tiempo; esto le pasa
     * al medidor lo que vio en un cuadro. El medidor deja de inferir: solo
     * guarda (y alimenta el clip si hay una ventana abierta para esa camara).
     * @param dets        detecciones de ese cuadro, en pixeles normalizados
     * @param deviceFrame buffer UYVY en device, solo para el clip (puede ser null)
     */
    void OnDetections(int camera, int64_t epochMs,
                      const std::vector<BallDetection>& dets,
                      const void* deviceFrame, size_t frameBytes,
                      unsigned int width, unsigned int height);

    /**
     * Arranca la carrera: marca el instante de inicio, vacia el anillo de las 4
     * camaras (deques + contadores de bytes) y deja la marca "grabando carrera"
     * para que OnDetections empiece a empujar cuadros. NO hace E/S: el volcado y
     * ffmpeg son de DumpRace().
     *
     * Se llama desde el motor de coreografia cuando se abre la compuerta
     * (ESP32Command "iniciar"). Con save_race=false es un no-op (no hay anillo
     * reservado ni marca que poner).
     */
    void RaceStart();

    /**
     * Vuelca a MP4 la carrera completa que esta en RAM (una por camara) y libera
     * el anillo de esa camara. NO bloquea: el volcado del crudo a disco y ffmpeg
     * corren en un hilo aparte por camara.
     *
     * Es publico a proposito. Se llama solo (una vez) al cumplirse race_seconds
     * desde RaceStart, y en Shutdown() como red de seguridad. Al volcar deja
     * m_raceActiva=false y los anillos vacios, asi que una segunda llamada (p.
     * ej. el Shutdown) no vuelve a volcar lo mismo.
     * Con save_race=false es un no-op (no hay nada grabado).
     */
    void DumpRace();

    /**
     * Anillo de una camara (para que DeckLinkCapture lo llene).
     * @return nullptr si esa camara no tiene anillo
     */
    FrameRing* GetRing(int cameraID);

    /**
     * Olvida las identidades provisorias (llamar al inicio de cada carrera).
     */
    void ResetIdentity();

    bool IsInitialized() const { return m_initialized; }
    uint64_t GetMeasureCount() const { return m_measureCount.load(); }
    uint64_t GetDroppedCount() const { return m_droppedCount.load(); }

private:
    // Una deteccion con todo lo necesario para seguir la esfera y votar su clase.
    // x,y,w,h en PIXELES (se convierten en IngestFrame).
    struct FrameDetection {
        float x = 0.0f, y = 0.0f;
        float w = 0.0f, h = 0.0f;
        int   ballID = 0;      // clase 0..9 tal como la devuelve el motor
        float conf = 0.0f;
    };

    // Muestreo de un frame: instante, detecciones y a que esfera (track) quedo
    // asignada cada una. trackOf lo llena ResolvePositions.
    struct FrameSample {
        int64_t epochMs = 0;
        std::vector<FrameDetection> dets;
        std::vector<int> trackOf;
    };

    // Esfera fisica seguida por movimiento durante la rafaga. Acumula el voto de
    // clase de cada cuadro: la banda blanca de la 9 aparece en unos cuadros y en
    // otros no, y el voto es lo que la separa de la 1.
    struct SphereTrack {
        int id = 0;
        float lastX = 0.0f, lastY = 0.0f;
        float lastW = 0.0f, lastH = 0.0f;
        int frames = 0;
        std::vector<int>   votes   = std::vector<int>(kTargetSpheres, 0);
        std::vector<float> confSum = std::vector<float>(kTargetSpheres, 0.0f);
    };


    // Historia de detecciones por camara: lo que el pipeline vio, ya deduplicado
    // por cuadro. El medidor agrega sobre esto; no infiere nada.
    static constexpr size_t kHistoriaPorCamara = 900;   // 30 s a 30 fps
    struct MuestraCamara {
        int64_t epochMs = 0;
        std::vector<FrameDetection> dets;
    };
    std::array<std::deque<MuestraCamara>, kMaxCameras> m_historia;
    std::mutex m_historiaMutex;
    // Ventanas pedidas por la coreografia y todavia sin cuadros, UNA POR CAMARA
    // (-1 = sin ventana en esa camara). El clip NO se abre en Enqueue (hilo de la
    // linea de tiempo), se abre con el primer cuadro de ESA camara en
    // OnDetections.
    std::array<int, kMaxCameras> m_ventanaPunto;
    // El estado de los clips lo tocan 3 hilos (coreografia, captura, medidor).
    std::mutex m_clipMutex;
    bool m_clipToolOk = false;   // ffmpeg responde (probado UNA vez en Initialize)

    void SnapshotHistory(int camera, int64_t fromMs, int64_t toMs,
                         std::vector<FrameSample>& out);

    void WorkerThreadFunc(std::stop_token stopToken);
    bool PopJob(MeasureJob& job);
    void ProcessJob(const MeasureJob& job);


    bool ResolvePositions(std::vector<FrameSample>& samples,
                          std::vector<int>& posiciones, int64_t& resolvedEpochMs);
    void MatchFrameByMotion(FrameSample& sample);
    static int ArgMaxClass(const SphereTrack& track);
    static int MaxDetections(const std::vector<FrameSample>& samples);
    static int64_t NowEpochMs();

    bool m_initialized = false;

    // Evidencia: grabacion de la rafaga (ver Clip* en el .cpp). No bloquea la
    // medicion: si algo falla se corta la grabacion y el punto sigue igual.
    //
    // UN contexto por camara: dos ventanas solapadas en camaras distintas graban
    // cada una su MP4. Todo lo que hay aca se toca con m_clipMutex tomado.
    // saveMaxFrames es el unico tope (no se cierra el clip mas viejo: hay RAM de
    // sobra y cerrarlo perderia el video de una medicion viva).
    struct ClipCtx {
        bool recording = false;
        bool failed = false;
        int frames = 0;
        int punto = 0;
        unsigned int w = 0;
        unsigned int h = 0;
        std::string dir;
        std::vector<unsigned char> host;    // staging del cuadro (device -> host)
        // Cuadros de la rafaga acumulados en RAM. En ClipStop van enteros al
        // stdin de ffmpeg (no se escribe ningun .uyvy): escribir 4 MB por cuadro
        // DENTRO del bucle de recoleccion hacia que el medidor no llegue a
        // copiar del anillo y perdiera cuadros (medido 3-nov: 1 cuadro de 150 en
        // una rafaga de 5 s).
        std::vector<unsigned char> buffer;
        // Instante (epoch ms) del PRIMER cuadro del clip de ESA camara. Es la
        // referencia de los tiempos del .ass de las cajas (draw_boxes): el MP4
        // arranca en ese cuadro, asi que todo tiempo dibujado es
        // epochMs - clip.firstMs.
        int64_t firstMs = 0;
    };
    std::array<ClipCtx, kMaxCameras> m_clips;
    std::string m_clipFfmpeg;      // ruta de ffmpeg YA expandida (%VAR% resuelto en C++)

    // Se llama con m_clipMutex TOMADO (viene de OnDetections). epochMs es el
    // instante del primer cuadro del clip: referencia de los tiempos del .ass.
    void ClipStartLocked(int camera, int punto, unsigned int width, unsigned int height,
                         int64_t epochMs);
    // Toma el lock y solo empuja si ESA camara esta grabando.
    void ClipPush(int camera, const void* devicePtr, size_t bytes);
    // Toma el lock solo para sacar copias; el volcado a ffmpeg (por stdin, sin
    // archivo crudo) va FUERA del lock. Nunca se llama teniendo m_clipMutex
    // tomado.
    void ClipStop(int camera);

    // ------------------------------------------------------------------------
    // Carrera completa en RAM (save_race). Un anillo por camara, acotado por
    // race_max_seconds: al pasarse del tope se descartan los cuadros MAS VIEJOS.
    //
    // Mutex PROPIO (m_raceMutex), distinto de m_clipMutex: el clip es la ventana
    // de 2 s de un punto y esto es la carrera entera. Nunca se toman los dos a
    // la vez (en OnDetections el bloque del clip cierra su lock antes de esto).
    // ------------------------------------------------------------------------
    struct RaceFrame {
        int64_t epochMs = 0;
        std::vector<unsigned char> datos;   // UYVY tal cual vino del device
        // Detecciones de ESE cuadro en PIXELES, solo para quemar las cajas en el
        // MP4 (draw_boxes). Vacio con draw_boxes=false: no se guarda ni una caja.
        std::vector<FrameDetection> dets;
    };
    std::array<std::deque<RaceFrame>, kMaxCameras> m_race;
    std::array<size_t, kMaxCameras> m_raceBytes{};      // bytes vivos por camara
    std::array<unsigned int, kMaxCameras> m_raceW{};
    std::array<unsigned int, kMaxCameras> m_raceH{};
    std::mutex m_raceMutex;
    // Marca "grabando carrera": la prende RaceStart() (compuerta) y la apaga el
    // corte automatico a los race_seconds (o DumpRace). Mientras este en false NO
    // se copia un cuadro al anillo, asi que el anillo no se llena antes de la
    // compuerta ni se vuelve a llenar despues del volcado. Es atomica porque la
    // lee el hilo de captura de cada camara sin tomar m_raceMutex (mismo criterio
    // que m_stopRequested).
    std::atomic<bool> m_raceActiva{false};
    // Instante (epoch ms) en que RaceStart() abrio la carrera. Se escribe una vez
    // por carrera, antes de poner m_raceActiva=true. Atomica por el mismo motivo
    // que la marca: la leen los hilos de captura sin tomar m_raceMutex (y una
    // segunda carrera puede reescribirla mientras miran).
    std::atomic<int64_t> m_raceInicioMs{0};

    // Volcados en curso, compartidos con los hilos de volcado por shared_ptr:
    // asi un hilo de volcado NO toca `this` y no puede quedar apuntando a un
    // medidor ya destruido.
    struct RaceDumpGate {
        std::mutex m;
        std::condition_variable cv;
        int inFlight = 0;
    };
    std::shared_ptr<RaceDumpGate> m_raceGate;

    void RacePush(int camera, const void* devicePtr, size_t bytes, int64_t epochMs,
                  unsigned int width, unsigned int height,
                  const std::vector<BallDetection>& dets);
    // Estatico: NO usa `this` (corre desprendido del objeto).
    static void RaceDumpWorker(std::deque<RaceFrame> cuadros, int camera,
                               unsigned int width, unsigned int height, int fps,
                               bool drawBoxes, std::string ffmpeg,
                               std::string mp4, std::string ass, std::string logf,
                               std::shared_ptr<RaceDumpGate> gate);

    // Anillos por camara
    std::array<std::unique_ptr<FrameRing>, kMaxCameras> m_rings;

    // Cola de mediciones (tope fijo, descarte del mas viejo)
    std::deque<MeasureJob> m_queue;
    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCv;

    // Hilo medidor
    std::jthread m_worker;

    // Publisher UDP (1 datagrama por punto)
    std::unique_ptr<RankingPublisher> m_publisher;

    // Defaults de medicion (bloque "ranking_measure"); el evento gana si trae
    // burst_ms/max_ms propios.
    RankingMeasureConfig m_config;

    // Inferencia propia (contexto de ejecucion propio, NO la de las PTZ)
    InferenceEngine m_inferenceEngine;
    bool m_inferenceReady = false;
    cudaStream_t m_inferenceStream = nullptr;
    void* m_deviceFrame = nullptr;      // buffer propio para el frame copiado
    size_t m_deviceFrameBytes = 0;

    // Identidades provisorias (persisten entre puntos de la misma carrera)
    std::vector<SphereTrack> m_tracks;
    int m_nextTrackId = 1;

    // Metricas
    std::atomic<uint64_t> m_measureCount{0};
    std::atomic<uint64_t> m_droppedCount{0};
    std::atomic<bool> m_stopRequested{false};   // corte del hilo medidor en shutdown

    // --- Observadores externos (API de control) -----------------------------
    // jobId de cada trabajo encolado: /arm empareja SU sonda con SU resultado.
    std::atomic<int64_t> m_nextJobId{1};

    // Callback de resultados. Se copia bajo m_resultCbMutex en cada uso para
    // que SetResultCallback(nullptr) durante el shutdown no corra una carrera
    // con el hilo medidor.
    mutable std::mutex m_resultCbMutex;
    MeasureResultCallback m_resultCallback;

    // Ultimo ranking publicado (copia thread-safe). Lo escribe el hilo medidor
    // al publicar el punto; lo lee la API de control.
    mutable std::mutex m_lastRankingMutex;
    std::vector<int> m_lastRanking;
    int m_lastRankingPunto = 0;
    int64_t m_lastRankingEpochMs = 0;

    void ReportResult(const MeasureResult& result);
};

/**
 * Punto de enganche unico con el motor de coreografia.
 *
 * El `case EventType::MeasurePoint` hace:
 *     if (auto* c = Measure::AttachedCoordinator()) c->Enqueue(...);
 * y vuelve. Si no hay medidor enganchado el evento es un no-op.
 */
RankingMeasure* AttachedCoordinator();
void AttachCoordinator(RankingMeasure* coordinator);

} // namespace Measure
