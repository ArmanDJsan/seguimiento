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
 *                   hilo medidor: frames -> inferencia -> orden -> 1x sendto()
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
 *  - Inferencia con instancia propia de InferenceEngine (contexto de ejecucion
 *    y buffers propios): NO se reusa el camino de inferencia de las 4 PTZ.
 *  - Sin ROI: se infiere el frame completo 1080p (el recorte es fase 2).
 */

#pragma once

#include <cuda_runtime.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
    // Ejecutable de ffmpeg. Puede ser "ffmpeg" (PATH) o una ruta completa.
    // Se admite %LOCALAPPDATA% y demas variables: las expande cmd, asi que la
    // ruta puede tener acentos sin problema de codificacion en el codigo.
    std::string ffmpegCmd = "ffmpeg";
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
};

/**
 * RankingMeasure - duenio del anillo, la cola, el hilo medidor, la
 * inferencia propia y el publisher UDP.
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

    RankingMeasure();
    ~RankingMeasure();

    RankingMeasure(const RankingMeasure&) = delete;
    RankingMeasure& operator=(const RankingMeasure&) = delete;

    /**
     * Crea anillos, publisher, stream de inferencia e instancia propia de
     * InferenceEngine, y arranca el hilo medidor.
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
     */
    void Enqueue(int camera, int punto, int burstMs, int maxMs);

    /**
     * Crea (idempotente) el anillo de una camara y el buffer de device propio
     * del medidor. Se llama desde el enganche de captura, cuando ya se conocen
     * las dimensiones reales del canal.
     * @return anillo de esa camara (nullptr si la camara esta fuera de rango)
     */
    FrameRing* SetupCamera(int cameraID, unsigned int width, unsigned int height);

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

    void WorkerThreadFunc(std::stop_token stopToken);
    bool PopJob(MeasureJob& job);
    void ProcessJob(const MeasureJob& job);

    bool IngestFrame(FrameRing* ring, int64_t index, FrameSample& out);
    int64_t CollectWindow(FrameRing* ring, int64_t fromIndex, int64_t fromTs,
                          int64_t untilTs, std::vector<FrameSample>& samples,
                          bool stopWhenComplete);

    bool ResolvePositions(std::vector<FrameSample>& samples,
                          std::vector<int>& posiciones, int64_t& resolvedEpochMs);
    void MatchFrameByMotion(FrameSample& sample);
    static int ArgMaxClass(const SphereTrack& track);
    static int MaxDetections(const std::vector<FrameSample>& samples);
    static int64_t NowEpochMs();

    bool m_initialized = false;

    // Evidencia: grabacion de la rafaga (ver Clip* en el .cpp). No bloquea la
    // medicion: si algo falla se corta la grabacion y el punto sigue igual.
    bool m_clipRecording = false;
    bool m_clipFailed = false;
    int m_clipFrames = 0;
    int m_clipPunto = 0;
    unsigned int m_clipW = 0;
    unsigned int m_clipH = 0;
    std::string m_clipDir;
    std::string m_clipFfmpeg;      // ruta de ffmpeg YA expandida (%VAR% resuelto en C++)
    std::vector<unsigned char> m_clipHost;
    // Cuadros de la rafaga acumulados en RAM. Se escriben a disco de una sola vez
    // en ClipStop: escribir 4 MB por cuadro DENTRO del bucle de recoleccion hacia
    // que el medidor no llegue a copiar del anillo y perdiera cuadros (medido
    // 3-nov: 1 cuadro de 150 en una rafaga de 5 s).
    std::vector<unsigned char> m_clipBuffer;

    void ClipStart(int punto, unsigned int width, unsigned int height);
    void ClipPush(const void* devicePtr, size_t bytes);
    void ClipStop();

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
