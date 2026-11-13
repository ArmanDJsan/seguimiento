/**
 * RankingMeasure.cpp
 *
 * Medidor por evento. Ver RankingMeasure.h para el contrato y las reglas duras.
 *
 * Flujo por evento:
 *   1. El motor de coreografia encola (Enqueue: anota la ventana de ESA camara y
 *      hace un push a una cola de tope 8). Nada de E/S en este hilo.
 *   2. El pipeline ya infiere cada cuadro de las 4 camaras y lo entrega por
 *      OnDetections: ahi se guarda la historia de esa camara y, si tiene ventana,
 *      se abre su clip y se empujan los cuadros crudos.
 *   3. El hilo medidor espera el fin de la ventana (burst_ms, luego max_ms) y
 *      resuelve el orden sobre la historia: emparejar esferas por movimiento y
 *      votar la clase de cada una.
 *   4. Cierra el clip de SU camara (ClipStop) y manda UN sendto() por punto.
 *
 * El clip es POR CAMARA (m_clips[camera]): dos mediciones solapadas en camaras
 * distintas no se pisan.
 */

#include "RankingMeasure.h"
#include "../json.hpp"

#include <algorithm>
#include <set>
#include "../core/PipelineGate.h"
#include "../utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>
#include <thread>
#include <string>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace Measure {

namespace {

constexpr int kPollIntervalMs = 5;      // cadencia de sondeo del hilo medidor
constexpr int kFinalDrainMs = 40;       // margen para el ultimo frame (~1 frame @30fps)
constexpr int kTimestampRetries = 50;   // reintentos de lectura de un slot en escritura
constexpr size_t kMaxTracks = 400;      // techo de identidades provisorias (era 40: saturaba en 10 cuadros)

// Cuadros CONSECUTIVOS con el minimo de esferas para abrir una ventana:
// un cuadro suelto (fantasma, reflejo) no arranca la toma.
constexpr int kCuadrosArranque = 3;

// Hasta donde se busca hacia atras el evento cuando el trabajo espero en la cola.
// El medidor es UN hilo: un punto puede arrancar 10-20 s despues de que el grupo
// paso, y el evento ya no esta en el cuadro actual.
constexpr int64_t kBusquedaAtrasMs = 30000;

// Tope de avance de una esfera sobre el recorrido, por cuadro (recorrido
// normalizado: 0,08 ~ 154 px de 1920). Medido en la carrera 20261109-160220:
// el avance real por cuadro tiene mediana 0,000 / p90 0,010 / p99 0,024, asi
// que 0,08 deja 3x de margen y solo corta los saltos de tramo (>0,5 medidos).
constexpr double kMaxPasoArco = 0.08;

std::atomic<RankingMeasure*> g_attachedCoordinator{nullptr};

// Definida mas abajo en este mismo namespace anonimo; se declara aca porque
// Initialize() la usa antes de su definicion.
std::string ExpandEnvVars(const std::string& in);

// Definida mas abajo en este mismo namespace anonimo; se declara aca porque
// ClipStop() (que esta antes) la usa. Lanza ffmpeg con los cuadros por STDIN
// (-i -), sin escribir ningun archivo de video crudo en disco.
int RunFfmpegWithStdin(const std::string& argsSinInput,
                       const std::vector<const std::vector<unsigned char>*>& cuadros,
                       const std::string& logf, std::string& salida);

} // namespace

// ============================================================================
// FrameRing
// ============================================================================

FrameRing::~FrameRing() {
    Shutdown();
}

bool FrameRing::Initialize(int cameraID, unsigned int width, unsigned int height) {
    if (m_initialized) {
        return true;
    }
    if (width == 0 || height == 0) {
        Logger::Error("FrameRing: dimensiones invalidas (" + std::to_string(width) + "x" +
                      std::to_string(height) + ")");
        return false;
    }

    m_cameraID = cameraID;
    m_width = width;
    m_height = height;
    m_frameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 2;  // UYVY = 2 B/px

    for (int i = 0; i < kSlots; ++i) {
        void* ptr = nullptr;
        cudaError_t err = cudaHostAlloc(&ptr, m_frameBytes, cudaHostAllocDefault);
        if (err != cudaSuccess) {
            Logger::Error("FrameRing: cudaHostAlloc fallo en camara " + std::to_string(cameraID) +
                          ": " + std::string(cudaGetErrorString(err)));
            Shutdown();
            return false;
        }
        m_slots[i].data = ptr;
        m_slots[i].epochMs = 0;
        m_slots[i].index = -1;
        m_slots[i].seq.store(0, std::memory_order_relaxed);
    }

    m_nextIndex.store(0, std::memory_order_release);
    m_initialized = true;

    const double megabytes = static_cast<double>(m_frameBytes) * kSlots / (1024.0 * 1024.0);
    Logger::Info("FrameRing: camara " + std::to_string(cameraID) + " anillo de " +
                 std::to_string(kSlots) + " frames pinned (" + std::to_string(megabytes) +
                 " MB, " + std::to_string(width) + "x" + std::to_string(height) + " UYVY)");
    return true;
}

void FrameRing::Shutdown() {
    for (int i = 0; i < kSlots; ++i) {
        if (m_slots[i].data) {
            cudaFreeHost(m_slots[i].data);
            m_slots[i].data = nullptr;
        }
        m_slots[i].epochMs = 0;
        m_slots[i].index = -1;
    }
    m_initialized = false;
    m_nextIndex.store(0, std::memory_order_release);
}

void FrameRing::Push(const void* src, size_t bytes, int64_t epochMs) {
    if (!m_initialized || !src || bytes == 0) {
        return;
    }

    const size_t copyBytes = (bytes < m_frameBytes) ? bytes : m_frameBytes;
    const int64_t index = m_nextIndex.load(std::memory_order_relaxed);
    Slot& slot = m_slots[static_cast<size_t>(index % kSlots)];

    // seqlock: impar = escritura en curso, par = slot estable
    const uint64_t seq = slot.seq.load(std::memory_order_relaxed);
    slot.seq.store(seq + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(slot.data, src, copyBytes);
    slot.epochMs = epochMs;
    slot.index = index;                 // el slot queda atado a ESTE frame
    slot.seq.store(seq + 2, std::memory_order_release);

    m_nextIndex.store(index + 1, std::memory_order_release);
}

bool FrameRing::TryGetTimestamp(int64_t index, int64_t& epochMs) const {
    if (!m_initialized || index < 0) {
        return false;
    }

    const int64_t latest = LatestIndex();
    if (index > latest || index <= latest - kSlots) {
        return false;   // todavia no existe, o el slot ya fue reciclado
    }

    const Slot& slot = m_slots[static_cast<size_t>(index % kSlots)];
    const uint64_t seqBefore = slot.seq.load(std::memory_order_acquire);
    if (seqBefore & 1ULL) {
        return false;   // escritura en curso
    }
    const int64_t ts = slot.epochMs;
    const int64_t slotIndex = slot.index;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot.seq.load(std::memory_order_acquire) != seqBefore) {
        return false;   // se sobrescribio durante la lectura
    }
    if (slotIndex != index) {
        return false;   // el slot ya fue reciclado por otro frame
    }

    epochMs = ts;
    return true;
}

bool FrameRing::CopyToDevice(int64_t index, void* deviceDst, size_t dstBytes,
                             cudaStream_t stream, int64_t& epochMs) const {
    if (!m_initialized || !deviceDst || index < 0) {
        return false;
    }

    const int64_t latest = LatestIndex();
    if (index > latest || index <= latest - kSlots) {
        return false;
    }

    const Slot& slot = m_slots[static_cast<size_t>(index % kSlots)];
    const size_t copyBytes = (dstBytes < m_frameBytes) ? dstBytes : m_frameBytes;

    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint64_t seqBefore = slot.seq.load(std::memory_order_acquire);
        if (seqBefore & 1ULL) {
            continue;   // escritura en curso, reintentar
        }
        const int64_t ts = slot.epochMs;

        cudaError_t err = cudaMemcpyAsync(deviceDst, slot.data, copyBytes,
                                          cudaMemcpyHostToDevice, stream);
        if (err != cudaSuccess) {
            Logger::Error("FrameRing: cudaMemcpyAsync fallo: " +
                          std::string(cudaGetErrorString(err)));
            return false;
        }
        // La validacion del seqlock necesita la copia terminada.
        if (cudaStreamSynchronize(stream) != cudaSuccess) {
            Logger::Error("FrameRing: cudaStreamSynchronize fallo durante la copia del frame");
            return false;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.seq.load(std::memory_order_acquire) == seqBefore && slot.index == index) {
            epochMs = ts;
            return true;
        }
    }

    return false;   // el slot se sobrescribio 3 veces: frame descartado
}

// ============================================================================
// Punto de enganche con el motor de coreografia
// ============================================================================

RankingMeasure* AttachedCoordinator() {
    return g_attachedCoordinator.load(std::memory_order_acquire);
}

void AttachCoordinator(RankingMeasure* coordinator) {
    g_attachedCoordinator.store(coordinator, std::memory_order_release);
}

void NotificarMovimiento(int camera) {
    RankingMeasure* c = g_attachedCoordinator.load(std::memory_order_acquire);
    if (c != nullptr) {
        c->MarcarMovimiento(camera);
    }
}

void RankingMeasure::SetPublishSink(std::function<void(const std::string&)> sink) {
    m_sinkPendiente = std::move(sink);
    if (m_publisher) {
        m_publisher->SetPublishSink(m_sinkPendiente);
    }
}

void RankingMeasure::SetRaceId(const std::string& id) {
    if (m_publisher) {
        m_publisher->SetRaceId(id);
    }
}

void RankingMeasure::MarcarMovimiento(int camera) {
    if (camera < 0 || camera >= kMaxCameras) {
        return;
    }
    const int64_t hasta = NowEpochMs() + m_config.ptzSettleMs;
    m_movHasta[camera].store(hasta, std::memory_order_relaxed);
    Logger::Info("RankingMeasure: camara " + std::to_string(camera + 1) +
                 " en movimiento " + std::to_string(m_config.ptzSettleMs) +
                 " ms (la toma no abre hasta que se asiente)");
}

// ============================================================================
// RankingMeasure
// ============================================================================

RankingMeasure::RankingMeasure() {
    // -1 = esa camara no tiene ventana pedida.
    m_ventanaPunto.fill(-1);
    for (auto& m : m_movHasta) {
        m.store(0);
    }
}

RankingMeasure::~RankingMeasure() {
    Shutdown();
}

bool RankingMeasure::Initialize(const RankingPublisherConfig& pubConfig,
                                const RankingMeasureConfig& measureConfig,
                                const InferenceEngineConfig& inferenceConfig) {
    if (m_initialized) {
        return true;
    }

    // 0. Defaults de medicion (el evento puede pisarlos con burst_ms/max_ms)
    m_config = measureConfig;
    m_filtrarPorRoi = measureConfig.filtrarRoi;
    m_paredRoi = measureConfig.paredRoi;
    // Direccion de avance de cada tramo (para ordenar por avance, no por x).
    LoadTrackVectors();
    (void)inferenceConfig;   // ya no hay motor propio: el pipeline infiere

    // save_race: el tope de RAM por camara no puede quedar en 0 (dejaria el
    // anillo sin un solo cuadro) ni sin techo. Rango: 1 s .. 1 h.
    if (m_config.raceMaxSeconds < 1) {
        m_config.raceMaxSeconds = 1;
    }
    if (m_config.raceMaxSeconds > 3600) {
        m_config.raceMaxSeconds = 3600;
    }
    // race_seconds: ventana real de la carrera (de la compuerta al volcado). Rango
    // 1 s .. 1 h. Si el tope del anillo quedo por debajo se sube al valor de la
    // carrera: si no, el anillo descartaria justo los cuadros que hay que volcar.
    if (m_config.raceSeconds < 1) {
        m_config.raceSeconds = 1;
    }
    if (m_config.raceSeconds > 3600) {
        m_config.raceSeconds = 3600;
    }
    if (m_config.raceMaxSeconds < m_config.raceSeconds) {
        m_config.raceMaxSeconds = m_config.raceSeconds;
    }
    // El cerrojo de volcados solo existe si hay carrera que grabar: con
    // save_race=false no se reserva absolutamente nada para esto.
    if (m_config.saveRace) {
        m_raceGate = std::make_shared<RaceDumpGate>();
    }

    // El probe de ffmpeg va UNA vez aca. Antes se hacia en ClipStart, que corre
    // en el hilo de la coreografia: arrancar un proceso ahi son decenas de ms
    // por punto y Enqueue tiene que ser instantaneo.
    // Se prueba tambien cuando solo esta pedido save_race: el volcado de la
    // carrera usa el mismo ejecutable.
    m_clipToolOk = false;
    if (m_config.saveClip || m_config.saveRace) {
        m_clipFfmpeg = ExpandEnvVars(m_config.ffmpegCmd);
        const std::string probe = "\"" + m_clipFfmpeg + "\" -version >nul 2>&1";
        m_clipToolOk = (system(probe.c_str()) == 0);
        if (m_clipToolOk) {
            Logger::Info("RankingMeasure: ffmpeg = " + m_clipFfmpeg);
        } else {
            Logger::Warning("RankingMeasure: save_clip/save_race encendido pero ffmpeg no responde (" +
                            m_clipFfmpeg + "); no se graban clips ni la carrera");
        }
    }

    // 1. Publisher UDP del punto (1 datagrama por punto)
    m_publisher = std::make_unique<RankingPublisher>(pubConfig);
    if (m_sinkPendiente) {
        m_publisher->SetPublishSink(m_sinkPendiente);
    }
    if (!m_publisher->Initialize()) {
        Logger::Error("RankingMeasure: RankingPublisher no inicializo, se aborta");
        m_publisher.reset();
        return false;
    }

    // 2. Ya NO hay motor propio: el pipeline infiere cada cuadro de cada camara
    //    todo el tiempo y nos pasa sus detecciones por OnDetections. Antes el
    //    medidor volvia a inferir los mismos cuadros con un segundo TensorRT,
    //    que era la mitad de la carga de GPU y la causa de que llegara tarde.
    // 5. Hilo medidor
    for (int cam = 0; cam < kMaxCameras; ++cam) {
        m_workers.emplace_back([this, cam](std::stop_token stopToken) {
            WorkerThreadFunc(cam, stopToken);
        });
    }

    m_initialized = true;
    m_stopRequested.store(false);
    Logger::Info("RankingMeasure: inicializado (cola tope=" +
                 std::to_string(kQueueCapacity) + ", anillo=" +
                 std::to_string(FrameRing::kSlots) + " frames/camara, sin ROI)");
    Logger::Info(std::string("RankingMeasure: save_race=") +
                 (m_config.saveRace ? "true" : "false") +
                 ", race_max_seconds=" + std::to_string(m_config.raceMaxSeconds) +
                 ", race_seconds=" + std::to_string(m_config.raceSeconds) +
                 (m_config.saveRace
                      ? (" (se graban " + std::to_string(m_config.raceSeconds) +
                         " s desde la compuerta; tope del anillo " +
                         std::to_string(m_config.raceMaxSeconds) +
                         " s x 30 fps por camara, se descarta lo mas viejo)")
                      : std::string(" (no se reserva RAM de carrera)")) +
                 ", draw_boxes=" + (m_config.drawBoxes ? "true" : "false") +
                 (m_config.drawBoxes
                      ? " (se queman las cajas del detector en los MP4 desde las "
                        "detecciones guardadas, al comprimir)"
                      : " (MP4 sin cajas, igual que siempre)"));
    return true;
}

void RankingMeasure::Shutdown() {
    // 1. Parar el hilo medidor antes de liberar anillos/GPU
    m_stopRequested.store(true);
    // Cerrar el clip de CADA camara: no debe quedar ninguna ventana ni evidencia
    // a medio volcar al apagar. ClipStop toma el lock por su cuenta y hace el
    // volcado fuera de el, asi que se puede llamar aca sin riesgo de deadlock.
    for (int cam = 0; cam < kMaxCameras; ++cam) {
        ClipStop(cam);
    }
    for (auto& w : m_workers) {
        if (w.joinable()) {
            w.request_stop();
        }
    }
    m_queueCv.notify_all();
    for (auto& w : m_workers) {
        if (w.joinable()) {
            w.join();
        }
    }
    m_workers.clear();

    // 1b. Red de seguridad: si la carrera siguio abierta (no se cumplio el corte
    //     automatico de race_seconds, p. ej. porque se cerro el programa antes),
    //     lo que quedo en RAM se vuelca ANTES de liberar. Si ya se volco, no hay
    //     anillos ni marca y esto no hace nada. El volcado a disco y ffmpeg corren
    //     en hilos aparte; aca se espera de forma acotada para que el proceso no
    //     termine con el MP4 a medio escribir.
    DumpRace();
    if (m_raceGate) {
        std::shared_ptr<RaceDumpGate> gate = m_raceGate;
        std::unique_lock<std::mutex> lock(gate->m);
        gate->cv.wait_for(lock, std::chrono::seconds(600),
                          [&gate] { return gate->inFlight == 0; });
        if (gate->inFlight != 0) {
            Logger::Warning("RankingMeasure: quedan " + std::to_string(gate->inFlight) +
                            " volcados de carrera sin terminar; se apaga igual");
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queues.clear();
    }

    // 2. Anillos pinned
    for (auto& ring : m_rings) {
        if (ring) {
            ring->Shutdown();
            ring.reset();
        }
    }

    // 5. Publisher UDP
    if (m_publisher) {
        m_publisher->Shutdown();
        m_publisher.reset();
    }

    m_initialized = false;
    Logger::Info("RankingMeasure: shutdown. Puntos medidos: " +
                 std::to_string(m_measureCount.load()) + ", descartados: " +
                 std::to_string(m_droppedCount.load()));
}

FrameRing* RankingMeasure::SetupCamera(int cameraID, unsigned int width,
                                               unsigned int height) {
    if (cameraID < 0 || cameraID >= kMaxCameras) {
        Logger::Warning("RankingMeasure: camara " + std::to_string(cameraID) +
                        " fuera de rango para medicion (0.." +
                        std::to_string(kMaxCameras - 1) + ")");
        return nullptr;
    }

    if (!m_rings[cameraID]) {
        m_rings[cameraID] = std::make_unique<FrameRing>();
        if (!m_rings[cameraID]->Initialize(cameraID, width, height)) {
            m_rings[cameraID].reset();
            return nullptr;
        }
    }

    return m_rings[cameraID].get();
}

FrameRing* RankingMeasure::GetRing(int cameraID) {
    if (cameraID < 0 || cameraID >= kMaxCameras) {
        return nullptr;
    }
    return m_rings[cameraID].get();
}

int64_t RankingMeasure::Enqueue(int camera, int punto, int burstMs, int maxMs, bool probe) {
    if (!m_initialized) {
        Logger::Warning("RankingMeasure: evento MeasurePoint ignorado (medidor no inicializado)");
        return 0;
    }
    // La ventana es un slot por camara: un indice fuera de rango seria memoria
    // invalida, y ademas no hay historia que muestrear para esa camara.
    if (camera < 0 || camera >= kMaxCameras) {
        Logger::Warning("RankingMeasure: evento MeasurePoint con camara " +
                        std::to_string(camera) + " fuera de rango (0.." +
                        std::to_string(kMaxCameras - 1) + "), se ignora");
        return 0;
    }

    MeasureJob job;
    job.jobId = m_nextJobId.fetch_add(1);
    job.camera = camera;
    job.punto = punto;
    job.probe = probe;
    job.burstMs = (burstMs > 0) ? burstMs : m_config.burstMs;
    job.maxMs = (maxMs > 0) ? maxMs : m_config.maxMs;
    if (job.maxMs < job.burstMs) {
        job.maxMs = job.burstMs;
    }
    job.requestedEpochMs = NowEpochMs();

    // La ventana de ESA camara se anota ANTES de encolar y SIN E/S (nada de
    // CreateDirectory/ffmpeg aca: este es el hilo de la linea de tiempo). El
    // worker no puede procesar el trabajo sin verla y el clip lo abre el hilo de
    // captura con el primer cuadro de la camara.
    // Una SONDA (API de control) no abre clip: no toca la ventana de la camara.
    if (!job.probe) {
        std::lock_guard<std::mutex> lock(m_clipMutex);
        m_ventanaPunto[camera] = job.punto;
    }

    {
        // Solo un push a la cola: nunca se espera al hilo medidor.
        std::lock_guard<std::mutex> lock(m_queueMutex);
        auto& cola = m_queues[job.camera];
        if (cola.size() >= kQueueCapacity) {
            const MeasureJob& dropped = cola.front();
            Logger::Warning("RankingMeasure: cola llena, se descarta la medicion mas vieja "
                            "(punto=" + std::to_string(dropped.punto) + ", camara=" +
                            std::to_string(dropped.camera) + ")");
            cola.pop_front();
            m_droppedCount++;
        }
        cola.push_back(job);
    }
    m_queueCv.notify_one();

    Logger::Debug("RankingMeasure: encolado punto=" + std::to_string(job.punto) +
                  " camara=" + std::to_string(job.camera) +
                  " burst=" + std::to_string(job.burstMs) + "ms" +
                  " max=" + std::to_string(job.maxMs) + "ms" +
                  (job.probe ? " (sonda)" : "") +
                  " jobId=" + std::to_string(job.jobId));
    return job.jobId;
}

int64_t RankingMeasure::EnqueueProbe(int camera, int burstMs, int maxMs) {
    // punto 0 = no es un punto del guion: la sonda no publica y no se registra
    // como punto medido. Solo reporta por el callback de resultados.
    return Enqueue(camera, 0, burstMs, maxMs, /*probe=*/true);
}

void RankingMeasure::SetResultCallback(MeasureResultCallback callback) {
    std::lock_guard<std::mutex> lock(m_resultCbMutex);
    m_resultCallback = std::move(callback);
}

bool RankingMeasure::GetLastRanking(std::vector<int>& posiciones, int& punto,
                                    int64_t& epochMs) const {
    std::lock_guard<std::mutex> lock(m_lastRankingMutex);
    posiciones = m_lastRanking;
    punto = m_lastRankingPunto;
    epochMs = m_lastRankingEpochMs;
    return !m_lastRanking.empty();
}

void RankingMeasure::ClearLastRanking() {
    {
        std::lock_guard<std::mutex> lock(m_lastRankingMutex);
        m_lastRanking.clear();
        m_lastRankingPunto = 0;
        m_lastRankingEpochMs = 0;
    }
    // La guarda de la compuerta no puede leer una medicion de la carrera
    // anterior: al arrancar un guion se borra tambien el ultimo resultado.
    std::lock_guard<std::mutex> lock(m_lastOutcomeMutex);
    m_lastOutcome = MeasureOutcome{};
    m_hasLastOutcome = false;
}

bool RankingMeasure::GetLastMeasureOutcome(MeasureOutcome& outcome) const {
    std::lock_guard<std::mutex> lock(m_lastOutcomeMutex);
    if (!m_hasLastOutcome) {
        outcome = MeasureOutcome{};
        return false;
    }
    outcome = m_lastOutcome;
    return true;
}

void RankingMeasure::ReportResult(const MeasureResult& result) {
    // La guarda de la compuerta (GateGuard.h) lee esto: se guarda la ultima
    // medicion TERMINADA de un punto del guion. Las sondas de /arm no entran:
    // no son un punto de la carrera y no publican.
    if (!result.probe) {
        {
            std::lock_guard<std::mutex> lock(m_lastOutcomeMutex);
            m_lastOutcome.punto = result.punto;
            m_lastOutcome.camera = result.camera;
            m_lastOutcome.spheres = result.spheres;
            m_lastOutcome.published = result.published;
            m_lastOutcome.motivo = result.motivo;
            m_lastOutcome.doneAtMs = NowEpochMs();
            m_hasLastOutcome = true;
        }
    }

    // Se invoca CON el lock tomado a proposito: asi SetResultCallback(nullptr)
    // (shutdown de la API de control) no vuelve mientras hay un callback en
    // curso, y el observador no puede quedar con un `this` ya destruido.
    std::lock_guard<std::mutex> lock(m_resultCbMutex);
    if (m_resultCallback) {
        m_resultCallback(result);
    }
}

void RankingMeasure::ResetIdentity() {
    for (auto& tabla : m_tracksCam) tabla.clear();
    // NO se reinicia m_nextTrackId a proposito: cada FrameSample ya guarda el id de
    // la pista en trackOf y despues se resuelve por id (posDeId). Si los ids vuelven
    // a 1, una deteccion vieja de una pista destruida se le atribuye a una pista
    // NUEVA y su voto (y su arco arrastrado) se mezclan entre segmentos. Medido: 5
    // resets dentro de la ventana del punto 7 (kMaxTracks=40).
    Logger::Info("RankingMeasure: identidades provisorias reiniciadas");
}

// ----------------------------------------------------------------------------
// Hilo medidor
// ----------------------------------------------------------------------------

void RankingMeasure::ActivarBanco(int punto, int camera, bool activo, bool sinRoi) {
    m_bancoSinRoi = sinRoi;
    if (activo) {
        if (m_bancoActivo.load()) {
            Logger::Info("Banco: ya estaba activo");
            return;
        }
        m_bancoPunto = punto;
        m_bancoCamera = camera;
        m_bancoActivo = true;
        Logger::Info("Banco: ACTIVADO punto=" + std::to_string(punto) +
                     " camara=" + std::to_string(camera) +
                     " (mide cuando entra el grupo y publica cuando va saliendo)");
        std::thread([this]() {
            int64_t ultimoJobMs = 0;
            while (!m_stopRequested.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (!m_bancoActivo.load()) continue;
                const int punto = m_bancoPunto.load();
                const int camara = m_bancoCamera.load();
                const int64_t ahora = NowEpochMs();
                // El banco necesita el pipeline despierto: sin carrera la captura y la
                // inferencia estan apagadas (ahorro). Se pide la ventana cada vuelta.
                Pipeline::PedirInferencia(120);
                if (ahora - ultimoJobMs < 3000) continue;
                std::vector<FrameSample> ult;
                SnapshotHistory(camara, ahora - 400, ahora, ult);
                if (ult.empty()) continue;
                const int enZona = EnZonaCuenta(camara, punto, ult.back());
                if (enZona < 1) {   // alcanza con que asome una: la primera es la entrada
                    // el grupo salio de la ROI: queda libre para la proxima pasada
                    m_bancoJobEnCurso = false;
                    continue;
                }
                if (m_bancoJobEnCurso.load()) {
                    // si el grupo no sale (esferas quietas), liberar a los 15 s
                    if (ahora - ultimoJobMs > 12000) m_bancoJobEnCurso = false;
                    continue;
                }
                MeasureJob job;
                job.punto = punto;
                job.camera = camara;
                job.requestedEpochMs = ahora;
                // ventana corta: con las esferas quietas el grupo nunca "sale", asi
                // que la toma se cierra por plazo y publica igual (~6 s de pista).
                job.burstMs = 10000;
                job.maxMs = 10000;
                ultimoJobMs = ahora;
                Logger::Info("Banco: entro el grupo a la ROI (" + std::to_string(enZona) +
                             " esferas), abre toma");
                {
                    std::lock_guard<std::mutex> lock(m_queueMutex);
                    auto& cola = m_queues[job.camera];
                    if (cola.size() >= kQueueCapacity) cola.pop_front();
                    cola.push_back(job);
                }
                m_queueCv.notify_one();
                m_bancoJobEnCurso = true;
            }
        }).detach();
        return;
    }
    if (!m_bancoActivo.exchange(false)) return;
    Logger::Info("Banco: DESACTIVADO");
}

void RankingMeasure::WorkerThreadFunc(int camera, std::stop_token stopToken) {
    Logger::Info("RankingMeasure: hilo medidor de camara " + std::to_string(camera + 1) + " iniciado");
    while (!stopToken.stop_requested()) {
        MeasureJob job;
        if (!PopJob(camera, job)) {
            continue;
        }
        if (stopToken.stop_requested()) {
            break;
        }
        ProcessJob(job);
    }
    Logger::Info("RankingMeasure: hilo medidor de camara " + std::to_string(camera + 1) + " detenido");
}

bool RankingMeasure::PopJob(int camera, MeasureJob& job) {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_queueCv.wait_for(lock, std::chrono::milliseconds(20), [this, camera] {
        const auto it = m_queues.find(camera);
        return it != m_queues.end() && !it->second.empty();
    });
    auto it = m_queues.find(camera);
    if (it == m_queues.end() || it->second.empty()) {
        return false;
    }
    job = it->second.front();
    it->second.pop_front();
    return true;
}

namespace {

std::string ClipStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    auto dos = [](unsigned v) { return (v < 10) ? ("0" + std::to_string(v)) : std::to_string(v); };
    return std::to_string(st.wYear) + dos(st.wMonth) + dos(st.wDay) + "-" +
           dos(st.wHour) + dos(st.wMinute) + dos(st.wSecond);
}

// Expande %VAR% con el entorno del proceso. No se delega a cmd porque dentro de
// _popen la expansion no ocurrio (medido 3-nov: el comando salio con
// %LOCALAPPDATA% literal y ffmpeg devolvio "no puede encontrar la ruta").
std::string ExpandEnvVars(const std::string& in) {
    std::string out;
    size_t i = 0;
    while (i < in.size()) {
        if (in[i] == '%') {
            const size_t j = in.find('%', i + 1);
            if (j != std::string::npos && j > i + 1) {
                const std::string name = in.substr(i + 1, j - i - 1);
                char* v = nullptr;
                size_t vLen = 0;
                if (_dupenv_s(&v, &vLen, name.c_str()) == 0 && v != nullptr) {
                    out += v;
                    free(v);
                    i = j + 1;
                    continue;
                }
            }
        }
        out += in[i];
        ++i;
    }
    return out;
}

std::string ClipPad4(int n) {
    std::string s = std::to_string(n);
    while (s.size() < 4) s = "0" + s;
    return s;
}

// ----------------------------------------------------------------------------
// Cajas del detector quemadas en el MP4 (draw_boxes)
//
// Nada de esto dibuja en la captura ni en el zero-copy: el MP4 se arma desde el
// crudo UYVY y las detecciones YA GUARDADAS, y las cajas entran como subtitulos
// ASS que ffmpeg quema al comprimir (-vf ass=...). Con draw_boxes=false no se
// llama a ninguna de estas funciones y el comando de ffmpeg no cambia.
// ----------------------------------------------------------------------------

// Una caja a dibujar: centro + tamano en PIXELES del cuadro (no normalizado).
struct AssBox {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    int   cls = 0;
    float conf = 0.0f;
};

// Las cajas de UN cuadro, con el instante RELATIVO al primer cuadro del video.
struct AssFrame {
    int64_t tMs = 0;
    std::vector<AssBox> boxes;
};

// Techo de cajas por cuadro: una salida patologica del modelo (miles de cajas)
// no puede inflar el .ass ni la RAM del anillo de la carrera.
constexpr size_t kMaxCajasPorCuadro = 64;

int AssRound(float v) {
    return static_cast<int>((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

// Tiempo de ASS: h:mm:ss.cc (centesimas). Ejemplo: 0:00:01.47
std::string AssTime(int64_t ms) {
    if (ms < 0) {
        ms = 0;
    }
    const int64_t cs = ms / 10;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%02lld",
                  static_cast<long long>(cs / 360000),
                  static_cast<long long>((cs / 6000) % 60),
                  static_cast<long long>((cs / 100) % 60),
                  static_cast<long long>(cs % 100));
    return std::string(buf);
}

// Escribe el .ass (PlayResX/Y = tamano real del cuadro, un estilo "caja" y un
// Dialogue de 33 ms por caja con el rectangulo dibujado por \p1 y la etiqueta
// "B<clase>" arriba a la izquierda). Devuelve false si no pudo escribirlo.
//
// El rectangulo se dibuja como CONTORNO (4 barras finas) y no como poligono
// relleno: una caja rellena taparia justo la esfera que se quiere mirar.
bool WriteAssFile(const std::string& path, unsigned int width, unsigned int height,
                  const std::vector<AssFrame>& frames) {
    if (path.empty() || width == 0 || height == 0) {
        return false;
    }
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || f == nullptr) {
        return false;
    }

    std::ostringstream oss;
    oss << "[Script Info]\r\n"
           "ScriptType: v4.00+\r\n"
           "PlayResX: " << width << "\r\n"
           "PlayResY: " << height << "\r\n"
           "WrapStyle: 2\r\n"
           "ScaledBorderAndShadow: yes\r\n"
           "\r\n[V4+ Styles]\r\n"
           "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
           "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
           "ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
           "MarginL, MarginR, MarginV, Encoding\r\n"
           "Style: caja,Arial,28,&H0000FFFF,&H0000FFFF,&H00000000,&H00000000,"
           "0,0,0,0,100,100,0,0,1,3,0,7,0,0,0,1\r\n"
           "\r\n[Events]\r\n"
           "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
           "Effect, Text\r\n";

    const int wMax = static_cast<int>(width) - 1;
    const int hMax = static_cast<int>(height) - 1;
    for (const auto& fr : frames) {
        const std::string ini = AssTime(fr.tMs);
        const std::string fin = AssTime(fr.tMs + 33);
        for (const auto& b : fr.boxes) {
            // x,y es el CENTRO de la caja (asi lo devuelve el motor); el
            // rectangulo se arma con medio ancho/alto a cada lado.
            int x0 = AssRound(b.x - b.w * 0.5f);
            int y0 = AssRound(b.y - b.h * 0.5f);
            int x1 = AssRound(b.x + b.w * 0.5f);
            int y1 = AssRound(b.y + b.h * 0.5f);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > wMax) x1 = wMax;
            if (y1 > hMax) y1 = hMax;
            if (x1 <= x0 || y1 <= y0) {
                continue;   // caja degenerada o fuera del cuadro
            }
            const int bw = x1 - x0;
            const int bh = y1 - y0;
            // Grosor del contorno: proporcional al alto del cuadro (4 px en
            // 1080p, 2 px en 720p), achicado en cajas chicas para que las cuatro
            // barras entren sin taparse entre si.
            int t = static_cast<int>(height) / 270;
            if (t < 2) t = 2;
            if (t > 6) t = 6;
            const int menor = (bw < bh) ? bw : bh;
            if (menor < 4 * t) {
                t = menor / 4;
                if (t < 1) t = 1;
            }

            // 1) Contorno: 4 barras (arriba, abajo, izquierda, derecha).
            oss << "Dialogue: 0," << ini << "," << fin << ",caja,,0,0,0,,"
                << "{\\an7\\pos(" << x0 << "," << y0 << ")\\p1\\bord0\\shad0"
                << "\\1c&H0000FFFF&}"
                << "m 0 0 l " << bw << " 0 l " << bw << " " << t << " l 0 " << t
                << "m 0 " << (bh - t) << " l " << bw << " " << (bh - t) << " l " << bw
                << " " << bh << " l 0 " << bh
                << "m 0 " << t << " l " << t << " " << t << " l " << t << " " << (bh - t)
                << " l 0 " << (bh - t)
                << "m " << (bw - t) << " " << t << " l " << bw << " " << t << " l " << bw
                << " " << (bh - t) << " l " << (bw - t) << " " << (bh - t) << "\r\n";

            // 2) Etiqueta "B<clase>" justo arriba de la caja, alineada a su
            // izquierda (\an1 = el texto termina en ese punto). Si la caja esta
            // pegada al borde de arriba, la etiqueta se queda dentro del cuadro.
            int etiquetaY = y0 - 2;
            if (etiquetaY < 30) etiquetaY = 30;
            oss << "Dialogue: 0," << ini << "," << fin << ",caja,,0,0,0,,"
                << "{\\an1\\pos(" << x0 << "," << etiquetaY
                << ")\\bord3\\shad0\\1c&H0000FFFF&}B" << b.cls << "\r\n";
        }
    }

    const std::string texto = oss.str();
    const size_t escritos = fwrite(texto.data(), 1, texto.size(), f);
    fclose(f);
    if (escritos != texto.size()) {
        DeleteFileA(path.c_str());
        return false;
    }
    return true;
}

// Ruta apta para el filtro "ass" de ffmpeg: barras normales (Windows las
// acepta) y los ':' escapados, porque libavfilter usa ':' como separador de
// opciones y '\\' como escape.
std::string AssFilterPath(const std::string& path) {
    std::string out;
    out.reserve(path.size() + 8);
    for (size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        if (c == '\\') {
            out += '/';
        } else if (c == ':') {
            out += "\\:";
        } else {
            out += c;
        }
    }
    return out;
}

// Trozo de linea de comando que quema el .ass. Vacio si no hay .ass: asi el
// comando de ffmpeg con draw_boxes=false queda caracter por caracter igual.
std::string AssFilterArgs(const std::string& assPath) {
    if (assPath.empty()) {
        return std::string();
    }
    return " -vf \"ass='" + AssFilterPath(assPath) + "'\"";
}

} // namespace

void RankingMeasure::ClipStartLocked(int camera, int punto, unsigned int width, unsigned int height,
                                     int64_t epochMs) {
    // Se llama con m_clipMutex TOMADO y con camera ya validada por OnDetections.
    ClipCtx& clip = m_clips[camera];
    clip.recording = false;
    clip.failed = false;
    clip.frames = 0;
    clip.punto = punto;
    clip.w = width;
    clip.h = height;
    // Referencia de los tiempos del .ass de las cajas: este es el primer cuadro
    // del MP4 de ESA camara (el que abre su clip).
    clip.firstMs = epochMs;
    clip.dir.clear();
    clip.buffer.clear();

    // ffmpeg ya se probo en Initialize: aca no se arranca ningun proceso.
    if (!m_config.saveClip || !m_clipToolOk) {
        return;
    }

    CreateDirectoryA(m_config.saveDir.c_str(), nullptr);
    // El nombre lleva la camara: dos ventanas solapadas pueden grabar el mismo
    // numero de punto y caer en el mismo segundo, y antes colisionaban (una
    // pisaba el MP4 de la otra).
    clip.dir = m_config.saveDir + "\\punto_" + std::to_string(punto) + "_cam" +
               std::to_string(camera) + "_" + ClipStamp();
    if (!CreateDirectoryA(clip.dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Logger::Warning("RankingMeasure: no se pudo crear la carpeta del clip (" + clip.dir + "), no se graba");
        clip.failed = true;
        return;
    }

    // UN SOLO flujo de cuadros, y va por un PIPE a ffmpeg (stdin): no se crea
    // ningun archivo crudo. Antes se escribia clip.uyvy (cientos de MB) y
    // Windows Defender lo bloqueaba recien escrito: ffmpeg devolvia rc=-13
    // (EACCES). Ademas eran GB de escritura y lectura por cada punto.
    // Se comprueba que la carpeta sea escribible con un archivo vacio de 0
    // bytes (el log de ffmpeg va ahi); los cuadros se acumulan en RAM.
    {
        const std::string sonda = clip.dir + "\\ffmpeg.log";
        HANDLE h = CreateFileA(sonda.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            Logger::Warning("RankingMeasure: la carpeta del clip no es escribible (" + clip.dir +
                            "), no se graba");
            clip.failed = true;
            return;
        }
        CloseHandle(h);
    }

    clip.recording = true;
    Logger::Info("RankingMeasure: grabando clip punto=" + std::to_string(punto) +
                 " camara=" + std::to_string(camera) +
                 " en " + clip.dir + " (max " + std::to_string(m_config.saveMaxFrames) + " cuadros)");
}

void RankingMeasure::ClipPush(int camera, const void* devicePtr, size_t bytes) {
    if (camera < 0 || camera >= kMaxCameras) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_clipMutex);
    ClipCtx& clip = m_clips[camera];
    if (!clip.recording || clip.failed || !devicePtr || bytes == 0) {
        return;
    }
    if (m_config.saveMaxFrames > 0 && clip.frames >= m_config.saveMaxFrames) {
        return;
    }
    if (clip.host.size() != bytes) {
        clip.host.resize(bytes);
    }
    if (cudaMemcpy(clip.host.data(), devicePtr, bytes, cudaMemcpyDeviceToHost) != cudaSuccess) {
        Logger::Warning("RankingMeasure: cudaMemcpy del clip fallo (camara " +
                        std::to_string(camera) + "), se corta la grabacion");
        clip.recording = false;
        clip.failed = true;
        return;
    }
    // A RAM, no a disco: el volcado va una sola vez en ClipStop.
    if (clip.buffer.capacity() == 0 && m_config.saveMaxFrames > 0) {
        clip.buffer.reserve(static_cast<size_t>(m_config.saveMaxFrames) * bytes);
    }
    clip.buffer.insert(clip.buffer.end(), clip.host.begin(), clip.host.end());
    ++clip.frames;
}

void RankingMeasure::ClipStop(int camera) {
    if (camera < 0 || camera >= kMaxCameras) {
        return;
    }
    // Se saca TODO el estado de ESA camara bajo el lock y se trabaja con copias:
    // mientras esto vuelca cientos de MB, el hilo de captura sigue empujando
    // cuadros de la ventana. El lock se suelta ANTES de tocar el disco y de
    // lanzar ffmpeg (nunca se llama a ClipStop con m_clipMutex tomado).
    std::string dir;
    std::vector<unsigned char> buffer;
    int cuadros = 0;
    int punto = 0;
    int cam = -1;
    int64_t firstMs = 0;
    unsigned int clipW = 0;
    unsigned int clipH = 0;
    {
        std::lock_guard<std::mutex> lock(m_clipMutex);
        ClipCtx& clip = m_clips[camera];
        // La ventana de esta camara ya vencio: no debe abrir el clip de un punto
        // posterior (el siguiente cuadro llegaria con otro numero de punto).
        m_ventanaPunto[camera] = -1;
        if (!clip.recording && clip.frames == 0) {
            return;
        }
        clip.recording = false;
        cuadros = clip.frames;
        clip.frames = 0;
        punto = clip.punto;
        firstMs = clip.firstMs;
        clipW = clip.w;
        clipH = clip.h;
        dir = clip.dir;
        buffer.swap(clip.buffer);
    }
    if (cuadros == 0) {
        return;
    }

    const std::string mp4 = dir + ".mp4";
    const std::string logf = dir + "\\ffmpeg.log";

    // Ni un byte a disco: el buffer de RAM va tal cual al stdin de ffmpeg. Antes
    // se volcaba clip.uyvy (~300 MB) y Defender bloqueaba ese archivo recien
    // escrito: ffmpeg devolvia rc=-13 (EACCES) y el punto se quedaba sin video.

    // Cajas del detector (draw_boxes): se arman AHORA, junto al buffer en RAM y
    // desde la historia de detecciones de ESA camara (misma ventana que el clip).
    // Los tiempos van relativos al PRIMER cuadro del clip: el MP4 arranca ahi y
    // el flujo crudo no tiene marcas de tiempo, la unica referencia valida es esa.
    // Con draw_boxes=false este bloque no corre y no se crea ningun .ass.
    std::string ass;
    if (m_config.drawBoxes && firstMs > 0 && clipW > 0 && clipH > 0) {
        const int fps = (m_config.saveFps > 0) ? m_config.saveFps : 30;
        const int64_t durMs = static_cast<int64_t>(cuadros) * 1000 / fps;
        std::vector<FrameSample> muestras;
        SnapshotHistory(camera, firstMs, firstMs + durMs + 100, muestras);

        std::vector<AssFrame> cajas;
        cajas.reserve(muestras.size());
        for (const auto& m : muestras) {
            int64_t t = m.epochMs - firstMs;
            if (t < 0) t = 0;
            if (t > durMs) t = durMs;    // nunca mas alla del ultimo cuadro
            AssFrame fr;
            fr.tMs = t;
            fr.boxes.reserve(m.dets.size());
            for (const auto& d : m.dets) {
                if (d.w <= 0.0f || d.h <= 0.0f) {
                    continue;
                }
                AssBox b;
                b.x = d.x;      // ya vienen en PIXELES (OnDetections convierte)
                b.y = d.y;
                b.w = d.w;
                b.h = d.h;
                b.cls = d.ballID;
                b.conf = d.conf;
                fr.boxes.push_back(b);
            }
            if (!fr.boxes.empty()) {
                cajas.push_back(std::move(fr));
            }
        }

        if (!cajas.empty()) {
            const std::string assPath = dir + "\\clip.ass";
            if (WriteAssFile(assPath, clipW, clipH, cajas)) {
                ass = assPath;
            } else {
                Logger::Warning("RankingMeasure: no se pudo escribir el .ass de cajas en " +
                                assPath + "; el clip se comprime sin cajas");
            }
        } else {
            Logger::Debug("RankingMeasure: clip punto=" + std::to_string(punto) +
                          " sin detecciones en la ventana; se comprime sin cajas");
        }
    }

    const std::string ffmpeg = m_clipFfmpeg.empty() ? ExpandEnvVars(m_config.ffmpegCmd) : m_clipFfmpeg;
    // La entrada NO esta en la linea: RunFfmpegWithStdin pone "-i -" en el
    // marcador {IN} y escribe los cuadros por el pipe. El filtro de cajas va
    // DESPUES de la entrada, como exige libavfilter.
    // Con draw_boxes=false AssFilterArgs("") devuelve vacio.
    const std::string cmdLine = "\"" + ffmpeg + "\" -y -v error -f rawvideo -pix_fmt uyvy422 -s " +
                                std::to_string(clipW) + "x" + std::to_string(clipH) +
                                " -framerate " + std::to_string(m_config.saveFps) +
                                " {IN}" + AssFilterArgs(ass) +
                                " -c:v libx264 -pix_fmt yuv420p -crf 20 \"" + mp4 + "\"";
    // Mismo comando sin el filtro: solo se usa como red si el ffmpeg del equipo
    // no trae el filtro "ass" (libass), para no perder el MP4 por las cajas.
    const std::string cmdSinCajas = "\"" + ffmpeg + "\" -y -v error -f rawvideo -pix_fmt uyvy422 -s " +
                                    std::to_string(clipW) + "x" + std::to_string(clipH) +
                                    " -framerate " + std::to_string(m_config.saveFps) +
                                    " {IN} -c:v libx264 -pix_fmt yuv420p -crf 20 \"" +
                                    mp4 + "\"";

    // El buffer del clip se pasa al hilo por shared_ptr: el hilo hace falta que
    // los bytes sigan vivos para el reintento sin cajas. Ya no hay copia extra
    // (antes se copiaba a un .uyvy en disco).
    std::shared_ptr<std::vector<unsigned char>> loteBytes =
        std::make_shared<std::vector<unsigned char>>(std::move(buffer));

    // El MP4 se arma en un hilo APARTE. ffmpeg tarda ~1 s y bloqueaba el hilo
    // medidor: la medicion siguiente arrancaba tarde y, como el anillo guarda 8
    // cuadros, los de su ventana ya no existian (medido 3-nov-2026: 3 de 9
    // puntos juntaron 1-9 cuadros y el clip salio vacio).
    std::thread([cmdLine, cmdSinCajas, loteBytes, mp4, logf, ass, cuadros, punto, camera]() {
        // Un solo bloque: el demuxer rawvideo parte el flujo en cuadros por
        // tamano (w*h*2). Nada de archivos intermedios.
        std::vector<const std::vector<unsigned char>*> lote;
        lote.push_back(loteBytes.get());

        std::string salida;
        int rc = RunFfmpegWithStdin(cmdLine, lote, logf, salida);

        // Si el ffmpeg del equipo no trae el filtro "ass" (libass) el MP4 no se
        // produce. Las cajas son diagnostico: se rehace SIN cajas antes que
        // quedarse sin la evidencia del punto. Los bytes siguen en memoria.
        if (rc != 0 && !ass.empty()) {
            Logger::Warning("RankingMeasure: ffmpeg con las cajas fallo (rc=" +
                            std::to_string(rc) + "); se rehace el clip punto=" +
                            std::to_string(punto) + " SIN cajas");
            salida.clear();
            rc = RunFfmpegWithStdin(cmdSinCajas, lote, logf, salida);
        }

        const bool mp4Ok = (rc == 0);
        if (mp4Ok) {
            DeleteFileA(logf.c_str());
            if (!ass.empty()) {
                DeleteFileA(ass.c_str());   // ya quedo quemado en el MP4
            }
        } else {
            Logger::Warning("RankingMeasure: ffmpeg fallo (rc=" + std::to_string(rc) +
                            ", bytes=" + std::to_string(loteBytes->size()) + ", cuadros=" +
                            std::to_string(cuadros) + "). Comando: " + cmdLine +
                            " . Salida: " + (salida.empty() ? std::string("(vacia)") : salida));
        }
        Logger::Info("RankingMeasure: clip punto=" + std::to_string(punto) +
                     " camara=" + std::to_string(camera) +
                     " cuadros=" + std::to_string(cuadros) + " mp4=" +
                     (mp4Ok ? std::string("si") : std::string("no")));
    }).detach();
}

// ============================================================================
// Carrera completa en RAM (save_race)
//
//   OnDetections (hilo del pipeline, por camara)
//        -> RacePush: cudaMemcpy D2H + push al deque de ESA camara
//                     (tope = race_max_seconds x 30 fps x bytes de cuadro;
//                      al pasarse se descartan los MAS VIEJOS)
//   DumpRace (corte automatico a los race_seconds desde la compuerta;
//             Shutdown solo como red de seguridad)
//        -> swap del deque de cada camara (libera el anillo)
//        -> hilo aparte por camara: cuadros por PIPE (stdin) + ffmpeg -> MP4
// ============================================================================

namespace {

// Cuadros por segundo que se asumen para acotar el anillo por tiempo. La
// captura de la pista corre a 30p, igual que save_fps por defecto.
constexpr int kRaceFps = 30;

// ----------------------------------------------------------------------------
// RunFfmpegWithStdin
//
// Lanza ffmpeg con los cuadros por STDIN (-i -) y la salida a un archivo de
// log. NO se escribe ningun archivo de video crudo: el pipe es la unica via.
// Motivo medido (4-nov-2026, cam0): el volcado escribia un .uyvy de ~7,4 GB y
// Windows Defender lo bloqueaba recien escrito; ffmpeg devolvia rc=-13 (EACCES,
// "Error opening input: Permission denied") incluso con 4 reintentos de 2 s y
// habia que convertir el crudo a mano. Ademas eran ~15 GB de escritura+lectura
// por carrera.
//
// argsSinInput: linea de ffmpeg SIN el archivo de entrada. En el lugar de la
// entrada lleva el marcador {IN}, que se reemplaza por "-i -" (asi el filtro de
// cajas queda DESPUES de la entrada, como exige libavfilter). Si el marcador no
// esta, "-i -" se agrega al final.
// cuadros: punteros a los bloques de bytes, en orden. Se escriben tal cual.
// Devuelve el codigo de salida de ffmpeg (-1 si no arranco).
// ----------------------------------------------------------------------------
int RunFfmpegWithStdin(const std::string& argsSinInput,
                       const std::vector<const std::vector<unsigned char>*>& cuadros,
                       const std::string& logf, std::string& salida) {
    std::string cmdLine = argsSinInput;
    const std::string marcador = "{IN}";
    const size_t pos = cmdLine.find(marcador);
    if (pos != std::string::npos) {
        cmdLine.replace(pos, marcador.size(), "-i -");
    } else {
        cmdLine += " -i -";
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = INVALID_HANDLE_VALUE;
    HANDLE hWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        salida = "CreatePipe fallo, error " + std::to_string(GetLastError());
        return -1;
    }
    // El hijo hereda SOLO el extremo de lectura (su stdin). Si heredara tambien
    // el de escritura, el pipe nunca veria EOF al cerrar el nuestro y ffmpeg se
    // quedaria esperando cuadros para siempre (el volcado se colgaria en vez de
    // cerrar el MP4).
    SetHandleInformation(hWrite, HANDLE_FLAG_INHERIT, 0);

    HANDLE hLog = CreateFileA(logf.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        Logger::Warning("RankingMeasure: no se pudo crear " + logf + " para la salida de ffmpeg");
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput  = hRead;
    si.hStdOutput = (hLog != INVALID_HANDLE_VALUE) ? hLog : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = (hLog != INVALID_HANDLE_VALUE) ? hLog : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back('\0');

    int rc = -1;
    bool cortado = false;
    DWORD errWrite = 0;
    long long escritos = 0;

    const BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) {
        salida = "CreateProcess fallo, error " + std::to_string(GetLastError());
        // CreateProcess fallo: nadie va a leer el pipe, se cierran los dos
        // extremos para no dejar handles colgados.
        CloseHandle(hWrite);
        CloseHandle(hRead);
    } else {
        // El padre ya no necesita el extremo de lectura: lo tiene el hijo.
        CloseHandle(hRead);

        // Escritura en el hilo que llama, en trozos: si ffmpeg muere, WriteFile
        // falla (ERROR_BROKEN_PIPE / ERROR_NO_DATA) y se corta en el acto en vez
        // de quedarse esperando. Nadie lee el buffer despues de esto.
        for (size_t i = 0; i < cuadros.size() && !cortado; ++i) {
            const std::vector<unsigned char>* c = cuadros[i];
            if (c == nullptr || c->empty()) {
                continue;
            }
            const unsigned char* p = c->data();
            size_t quedan = c->size();
            while (quedan > 0) {
                const DWORD trozo = static_cast<DWORD>(quedan > (1024u * 1024u)
                                                           ? (1024u * 1024u)
                                                           : quedan);
                DWORD n = 0;
                if (!WriteFile(hWrite, p, trozo, &n, nullptr) || n == 0) {
                    errWrite = GetLastError();
                    cortado = true;
                    break;
                }
                p += n;
                quedan -= n;
                escritos += static_cast<long long>(n);
            }
        }
        // Cerrar el extremo de escritura es el EOF que ffmpeg necesita para
        // terminar de comprimir y cerrar el MP4.
        CloseHandle(hWrite);

        // 10 min de techo: una carrera de 120 s son 3600 cuadros a comprimir.
        WaitForSingleObject(pi.hProcess, 600000);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        rc = static_cast<int>(code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (hLog != INVALID_HANDLE_VALUE) {
        CloseHandle(hLog);
    }

    FILE* f = nullptr;
    std::string log;
    if (fopen_s(&f, logf.c_str(), "rb") == 0 && f != nullptr) {
        char tmp[512];
        const size_t n = fread(tmp, 1, sizeof(tmp) - 1, f);
        tmp[n] = '\0';
        log = tmp;
        fclose(f);
    }
    for (size_t i = 0; i < log.size(); ++i) {
        if (log[i] == '\n' || log[i] == '\r' || log[i] == '\t') log[i] = ' ';
    }
    while (!log.empty() && log.back() == ' ') log.pop_back();

    if (cortado) {
        // ffmpeg murio a mitad del volcado: se reporta el error de escritura
        // junto con lo que haya dejado en el log (no se reintenta a ciegas).
        salida = "escritura al stdin de ffmpeg cortada (error " + std::to_string(errWrite) +
                 ", " + std::to_string(escritos) + " bytes escritos)" +
                 (log.empty() ? std::string() : (". Salida: " + log));
        if (rc == 0) {
            rc = -1;    // ffmpeg "termino bien" pero no recibio todo: MP4 incompleto
        }
    } else {
        salida += log;
    }
    return rc;
}

} // namespace

void RankingMeasure::RacePush(int camera, const void* devicePtr, size_t bytes,
                              int64_t epochMs, unsigned int width, unsigned int height,
                              const std::vector<BallDetection>& dets) {
    // Doble chequeo: con save_race=false no se reserva ni un byte ni se copia
    // nada (el primer chequeo ya esta en OnDetections, antes de llamar aca), y
    // con la carrera sin arrancar (m_raceActiva=false) tampoco: el anillo se
    // llena SOLO entre la compuerta y el corte de race_seconds.
    if (!m_config.saveRace || !m_raceActiva.load() || devicePtr == nullptr || bytes == 0) {
        return;
    }
    if (camera < 0 || camera >= kMaxCameras) {
        return;
    }
    // Techo de cuadro: un tamano inesperado se descarta ANTES de reservar.
    if (bytes > kMaxFrameBytes) {
        Logger::Warning("RankingMeasure: cuadro de " + std::to_string(bytes) +
                        " bytes excede el maximo de la carrera; se descarta");
        return;
    }

    // La copia D2H va FUERA del lock y en un buffer thread_local: el pipeline
    // puede llamar desde el hilo de cada camara y un buffer compartido seria una
    // carrera. Son ~4 MB por cuadro por camara (~500 MB/s las 4).
    static thread_local std::vector<unsigned char> host;
    if (host.size() != bytes) {
        host.resize(bytes);
    }
    if (cudaMemcpy(host.data(), devicePtr, bytes, cudaMemcpyDeviceToHost) != cudaSuccess) {
        Logger::Warning("RankingMeasure: cudaMemcpy de la carrera fallo; se pierde el cuadro");
        return;
    }

    // Tope de RAM de esa camara: race_max_seconds a 30 fps. Se mide con el
    // tamano real del cuadro (w*h*2 en UYVY); si no vinieron las dimensiones, con
    // los bytes copiados.
    size_t bytesPorCuadro = bytes;
    if (width > 0 && height > 0) {
        bytesPorCuadro = static_cast<size_t>(width) * static_cast<size_t>(height) * 2u;
    }
    const size_t capCuadros = static_cast<size_t>(m_config.raceMaxSeconds) *
                              static_cast<size_t>(kRaceFps);
    const size_t capBytes = capCuadros * bytesPorCuadro;

    RaceFrame f;
    f.epochMs = epochMs;
    f.datos.assign(host.begin(), host.end());

    // Cajas para quemar en el MP4 de la carrera (draw_boxes). Se guardan en
    // PIXELES y tal como las dio el modelo en este cuadro (sin dedupe: es lo que
    // el modelo vio). Con draw_boxes=false no se guarda ni una caja.
    if (m_config.drawBoxes && width > 0 && height > 0 && !dets.empty()) {
        const size_t n = (std::min)(dets.size(), kMaxCajasPorCuadro);
        const float W = static_cast<float>(width);
        const float H = static_cast<float>(height);
        f.dets.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            if (dets[i].width <= 0.0f || dets[i].height <= 0.0f) {
                continue;
            }
            FrameDetection d;
            d.x = dets[i].x * W;
            d.y = dets[i].y * H;
            d.w = dets[i].width * W;
            d.h = dets[i].height * H;
            d.ballID = dets[i].ballID;
            d.conf = dets[i].confidence;
            f.dets.push_back(d);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_raceMutex);
        const bool primero = m_race[camera].empty();
        m_race[camera].push_back(std::move(f));
        m_raceBytes[camera] += bytes;
        m_raceW[camera] = width;
        m_raceH[camera] = height;

        // Tope SIEMPRE: se descartan los cuadros mas viejos hasta entrar en el
        // presupuesto de bytes y de cuadros de esa camara.
        while (!m_race[camera].empty() &&
               (m_raceBytes[camera] > capBytes || m_race[camera].size() > capCuadros)) {
            m_raceBytes[camera] -= m_race[camera].front().datos.size();
            m_race[camera].pop_front();
        }
        if (primero) {
            Logger::Info("RankingMeasure: grabando carrera en RAM cam" +
                         std::to_string(camera) + " (tope " +
                         std::to_string(m_config.raceMaxSeconds) + " s)");
        }
    }
}

// ============================================================================
// Volcado de la INFERENCIA de la carrera (save_inference)
//
//   OnDetections (hilo de captura, por camara)
//        -> InfPush: copia las cajas del cuadro a un deque de ESA camara
//                    (~300 B por cuadro; sin E/S, sin pixeles)
//   DumpInference (al cerrar la carrera)
//        -> swap de los deques y un hilo aparte escribe un .jsonl por camara
//
// El archivo queda al lado de los MP4 y con el mismo sello de tiempo, asi que
// cada caja se puede cruzar con su cuadro por el campo "t" (epoch ms).
// ============================================================================

void RankingMeasure::InfReset() {
    if (!m_config.saveInference) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_infMutex);
        for (int cam = 0; cam < kMaxCameras; ++cam) {
            m_infRace[cam].clear();
        }
        // Mismo tope que el anillo de video: si la carrera se pasa, se descartan
        // los cuadros MAS VIEJOS (nunca se corta el final, que es la llegada).
        m_infMaxFrames = static_cast<size_t>((std::max)(30, m_config.raceMaxSeconds * 30));
        m_infActiva.store(true);
    }
    Logger::Info("RankingMeasure: inferencia: se guardan las cajas de la carrera (tope " +
                 std::to_string(m_infMaxFrames) + " cuadros por camara)");
}

void RankingMeasure::InfPush(int camera, const MuestraCamara& muestra) {
    // Chequeo sin lock: con save_inference apagado o fuera de carrera esto cuesta
    // una lectura atomica y no toca nada mas.
    if (!m_infActiva.load(std::memory_order_relaxed)) {
        return;
    }
    if (camera < 0 || camera >= kMaxCameras) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_infMutex);
    auto& cola = m_infRace[camera];
    cola.push_back(muestra);      // copia chica: solo el vector de cajas
    while (cola.size() > m_infMaxFrames) {
        cola.pop_front();
    }
}

void RankingMeasure::DumpInference() {
    std::array<std::deque<MuestraCamara>, kMaxCameras> copias;
    {
        std::lock_guard<std::mutex> lock(m_infMutex);
        m_infActiva.store(false);
        for (int cam = 0; cam < kMaxCameras; ++cam) {
            copias[cam].swap(m_infRace[cam]);   // el deque de la camara queda libre
        }
    }
    bool hay = false;
    for (int cam = 0; cam < kMaxCameras; ++cam) {
        if (!copias[cam].empty()) {
            hay = true;
        }
    }
    if (!hay) {
        return;   // sin cuadros (o ya volcado): no se escribe nada
    }

    CreateDirectoryA(m_config.saveDir.c_str(), nullptr);
    const std::string stamp = ClipStamp();
    const std::string dir = m_config.saveDir;

    // Se escribe ACA MISMO, no en un hilo aparte: son ~4 MB (5.400 lineas) y este
    // hilo es el del MEDIDOR, no el de captura ni el de inferencia, asi que no
    // frena el camino caliente. Escribirlo inline evita el riesgo de que cierren
    // el programa y el .jsonl quede a medio escribir (el MP4 si necesita hilo
    // aparte porque son GB).
    for (int cam = 0; cam < kMaxCameras; ++cam) {
        if (copias[cam].empty()) {
            continue;
        }
        const std::string path = dir + "\\inferencia_cam" + std::to_string(cam) + "_" + stamp + ".jsonl";
        std::ofstream f(path, std::ios::binary);
        if (!f) {
            Logger::Warning("RankingMeasure: no se pudo abrir " + path + " para la inferencia");
            continue;
        }
        for (const auto& m : copias[cam]) {
            f << "{\"t\":" << m.epochMs << ",\"cam\":" << cam
              << ",\"n\":" << m.dets.size() << ",\"dets\":[";
            for (size_t i = 0; i < m.dets.size(); ++i) {
                const FrameDetection& d = m.dets[i];
                if (i) {
                    f << ',';
                }
                f << "{\"cls\":" << d.ballID
                  << ",\"conf\":" << std::fixed << std::setprecision(4) << d.conf
                  << ",\"x\":" << std::setprecision(1) << d.x
                  << ",\"y\":" << d.y
                  << ",\"w\":" << d.w
                  << ",\"h\":" << d.h << '}';
            }
            f << "]}\n";
        }
        f.close();
        Logger::Info("RankingMeasure: inferencia cam" + std::to_string(cam) + " volcada (" +
                     std::to_string(copias[cam].size()) + " cuadros) en " + path);
    }
}

void RankingMeasure::RaceStart() {
    // El volcado de cajas no depende del video: se reinicia aunque save_race este
    // apagado (InfReset sale solo si save_inference esta apagado).
    InfReset();

    if (!m_config.saveRace) {
        return;   // sin save_race no hay anillo reservado ni marca que poner
    }

    int camaras = 0;
    {
        std::lock_guard<std::mutex> lock(m_raceMutex);
        for (int cam = 0; cam < kMaxCameras; ++cam) {
            // Vaciar el anillo de ESA camara: los cuadros (y sus buffers) se
            // sueltan y el contador de bytes vuelve a cero.
            m_race[cam].clear();
            m_raceBytes[cam] = 0;
            // Las dimensiones (m_raceW/m_raceH) se dejan como estan: las fija el
            // primer cuadro que empuje RacePush y sirven para no escribir un MP4
            // con la geometria equivocada.
            if (m_rings[cam] && m_rings[cam]->IsInitialized()) {
                ++camaras;
            }
        }
        // Instante de la compuerta: contra esto se mide el corte automatico.
        m_raceInicioMs.store(NowEpochMs());
        // Recien aca se abre la compuerta del anillo: hasta este instante
        // OnDetections NO copiaba ni un cuadro (el anillo no se llena antes).
        m_raceActiva.store(true);
    }

    Logger::Info("RankingMeasure: carrera: anillo reiniciado (camaras " +
                 std::to_string(camaras) + "), se vuelca solo a los " +
                 std::to_string(m_config.raceSeconds) + " s");
}

void RankingMeasure::DumpRace() {
    // Primero las cajas: no dependen del video y no deben perderse si save_race
    // esta apagado (o si el volcado del MP4 falla).
    if (m_config.saveInference) {
        DumpInference();
    }

    if (!m_config.saveRace) {
        return;   // nunca se grabo nada: no hay anillo que volcar
    }

    CreateDirectoryA(m_config.saveDir.c_str(), nullptr);

    {
        // Fin de carrera: la marca se apaga ANTES de vaciar los anillos, para que
        // el hilo de captura deje de empujar cuadros en el mismo instante del
        // swap. Si quedara encendida, un segundo DumpRace (el de Shutdown)
        // volveria a encontrar cuadros y los volcaria como otra carrera.
        std::lock_guard<std::mutex> lock(m_raceMutex);
        m_raceActiva.store(false);
    }

    for (int cam = 0; cam < kMaxCameras; ++cam) {
        std::deque<RaceFrame> cuadros;
        unsigned int w = 0;
        unsigned int h = 0;
        size_t tamanosDistintos = 0;
        int64_t primero = 0;
        int64_t ultimo = 0;
        {
            std::lock_guard<std::mutex> lock(m_raceMutex);
            if (m_race[cam].empty()) {
                continue;
            }
            cuadros.swap(m_race[cam]);      // el anillo de esta camara queda libre
            m_raceBytes[cam] = 0;
            w = m_raceW[cam];
            h = m_raceH[cam];
            primero = cuadros.front().epochMs;
            ultimo = cuadros.back().epochMs;
            const size_t tam = cuadros.front().datos.size();
            for (size_t i = 0; i < cuadros.size(); ++i) {
                if (cuadros[i].datos.size() != tam) {
                    ++tamanosDistintos;
                }
            }
        }

        if (w == 0 || h == 0) {
            // Sin dimensiones no hay forma de armar el raw: mejor no escribir un
            // MP4 con la geometria equivocada.
            Logger::Warning("RankingMeasure: carrera cam" + std::to_string(cam) +
                            " sin dimensiones; se descarta (" +
                            std::to_string(cuadros.size()) + " cuadros)");
            continue;
        }
        if (tamanosDistintos > 0) {
            Logger::Warning("RankingMeasure: carrera cam" + std::to_string(cam) + " con " +
                            std::to_string(tamanosDistintos) +
                            " cuadros de tamano distinto; el MP4 puede salir mal");
        }

        const std::string stamp = ClipStamp();
        const std::string mp4 = m_config.saveDir + "\\carrera_cam" + std::to_string(cam) +
                                "_" + stamp + ".mp4";
        // .ass de las cajas (draw_boxes): se escribe junto al MP4 y se borra si
        // el MP4 sale bien. Con draw_boxes=false la ruta queda vacia y el
        // comando de ffmpeg no cambia.
        const std::string ass = m_config.drawBoxes
                                    ? (m_config.saveDir + "\\carrera_cam" + std::to_string(cam) +
                                       "_" + stamp + ".ass")
                                    : std::string();
        const std::string logf = m_config.saveDir + "\\carrera_cam" + std::to_string(cam) +
                                 "_" + stamp + ".log";
        const std::string ffmpeg = m_clipFfmpeg.empty() ? ExpandEnvVars(m_config.ffmpegCmd)
                                                        : m_clipFfmpeg;

        std::shared_ptr<RaceDumpGate> gate = m_raceGate;
        if (!gate) {
            gate = std::make_shared<RaceDumpGate>();
        }
        {
            std::lock_guard<std::mutex> lock(gate->m);
            ++gate->inFlight;
        }

        Logger::Info("RankingMeasure: volcando carrera cam" + std::to_string(cam) +
                     " cuadros=" + std::to_string(cuadros.size()) + " (" +
                     std::to_string(primero) + ".." + std::to_string(ultimo) + " ms) -> " + mp4);

        // Los cuadros van a ffmpeg por un PIPE (stdin) dentro de un hilo APARTE:
        // el hilo medidor y el de captura no esperan. No se escribe ningun
        // archivo crudo. El hilo no toca `this`.
        try {
            std::thread(&RankingMeasure::RaceDumpWorker, std::move(cuadros), cam, w, h,
                        m_config.saveFps, m_config.drawBoxes, ffmpeg, mp4, ass, logf,
                        gate).detach();
        } catch (...) {
            std::lock_guard<std::mutex> lock(gate->m);
            --gate->inFlight;
            gate->cv.notify_all();
            Logger::Warning("RankingMeasure: no se pudo lanzar el volcado de la carrera cam" +
                            std::to_string(cam) + "; se descarta");
        }
    }
}

void RankingMeasure::RaceDumpWorker(std::deque<RaceFrame> cuadros, int camera,
                                    unsigned int width, unsigned int height, int fps,
                                    bool drawBoxes, std::string ffmpeg,
                                    std::string mp4, std::string ass, std::string logf,
                                    std::shared_ptr<RaceDumpGate> gate) {
    const size_t totalCuadros = cuadros.size();

    // Cajas del detector (draw_boxes): se arman ANTES de mandar los cuadros a
    // ffmpeg, y el .ass queda escrito antes de lanzar el proceso. Tiempos
    // relativos al PRIMER cuadro del video (el MP4 arranca ahi). Con
    // draw_boxes=false no se llama a nada de esto y el comando de ffmpeg no
    // cambia (AssFilterArgs("") devuelve vacio).
    if (drawBoxes && !ass.empty() && width > 0 && height > 0 && !cuadros.empty()) {
        const int fpsSeguro = (fps > 0) ? fps : kRaceFps;
        const int64_t primeroMs = cuadros.front().epochMs;
        const int64_t durMs = static_cast<int64_t>(cuadros.size()) * 1000 / fpsSeguro;

        std::vector<AssFrame> cajas;
        cajas.reserve(cuadros.size());
        for (const auto& cuadro : cuadros) {
            if (cuadro.dets.empty()) {
                continue;
            }
            int64_t t = cuadro.epochMs - primeroMs;
            if (t < 0) t = 0;
            if (t > durMs) t = durMs;
            AssFrame fr;
            fr.tMs = t;
            fr.boxes.reserve(cuadro.dets.size());
            for (const auto& d : cuadro.dets) {
                AssBox b;
                b.x = d.x;      // ya vienen en PIXELES (los convierte RacePush)
                b.y = d.y;
                b.w = d.w;
                b.h = d.h;
                b.cls = d.ballID;
                b.conf = d.conf;
                fr.boxes.push_back(b);
            }
            cajas.push_back(std::move(fr));
        }
        if (!cajas.empty() && !WriteAssFile(ass, width, height, cajas)) {
            Logger::Warning("RankingMeasure: no se pudo escribir el .ass de cajas de la carrera cam" +
                            std::to_string(camera) + " (" + ass + "); se comprime sin cajas");
            ass.clear();    // sin .ass no se agrega el filtro
        }
    }

    // 1. Nada de archivo crudo: se pasan los punteros a los cuadros y
    //    RunFfmpegWithStdin los escribe por el pipe. El deque NO se libera
    //    mientras dura el volcado: el reintento sin cajas necesita los mismos
    //    bytes, y la memoria ya estaba reservada por el anillo (con 60 s de
    //    1080p UYVY son los mismos ~7,4 GB de antes, sin la copia a disco).
    std::vector<const std::vector<unsigned char>*> lote;
    lote.reserve(cuadros.size());
    long long escritos = 0;
    for (const auto& cuadro : cuadros) {
        if (cuadro.datos.empty()) {
            continue;
        }
        lote.push_back(&cuadro.datos);
        escritos += static_cast<long long>(cuadro.datos.size());
    }

    bool mp4Ok = false;
    if (!lote.empty()) {
        // {IN} lo cambia el helper por "-i -": los cuadros llegan por stdin.
        const std::string cmdLine = "\"" + ffmpeg + "\" -y -v error -f rawvideo -pix_fmt uyvy422 -s " +
                                    std::to_string(width) + "x" + std::to_string(height) +
                                    " -framerate " + std::to_string(fps) +
                                    " {IN}" + AssFilterArgs(ass) +
                                    " -c:v libx264 -pix_fmt yuv420p -crf 20 \"" + mp4 + "\"";
        // Mismo comando sin el filtro: red por si el ffmpeg no trae libass.
        const std::string cmdSinCajas = "\"" + ffmpeg +
                                        "\" -y -v error -f rawvideo -pix_fmt uyvy422 -s " +
                                        std::to_string(width) + "x" + std::to_string(height) +
                                        " -framerate " + std::to_string(fps) +
                                        " {IN} -c:v libx264 -pix_fmt yuv420p -crf 20 \"" + mp4 + "\"";
        // Sin reintentos "por Defender": ya no hay archivo crudo recien escrito
        // que bloquear (era la causa de rc=-13 EACCES). Si ffmpeg falla, el
        // helper devuelve rc + el log y se rehace SIN cajas si habia .ass.
        std::string salida;
        int rc = RunFfmpegWithStdin(cmdLine, lote, logf, salida);
        // Si el ffmpeg del equipo no trae el filtro "ass" (libass), el MP4 no se
        // produce: las cajas son diagnostico, se rehace SIN cajas.
        if (rc != 0 && !ass.empty()) {
            Logger::Warning("RankingMeasure: ffmpeg con las cajas fallo en la carrera cam" +
                            std::to_string(camera) + " (rc=" + std::to_string(rc) +
                            "); se rehace SIN cajas");
            salida.clear();
            rc = RunFfmpegWithStdin(cmdSinCajas, lote, logf, salida);
        }
        mp4Ok = (rc == 0);
        if (mp4Ok) {
            DeleteFileA(logf.c_str());
            if (!ass.empty()) {
                DeleteFileA(ass.c_str()); // ya quedo quemado en el MP4
            }
        } else {
            Logger::Warning("RankingMeasure: ffmpeg de la carrera cam" +
                            std::to_string(camera) + " fallo (rc=" + std::to_string(rc) +
                            ", bytes=" + std::to_string(escritos) + ", cuadros=" +
                            std::to_string(totalCuadros) + "). Comando: " + cmdLine +
                            " . Salida: " + (salida.empty() ? std::string("(vacia)") : salida));
        }
    }

    Logger::Info("RankingMeasure: carrera cam" + std::to_string(camera) +
                 " cuadros=" + std::to_string(totalCuadros) + " mp4=" +
                 (mp4Ok ? std::string("si") : std::string("no")));

    if (gate) {
        std::lock_guard<std::mutex> lock(gate->m);
        --gate->inFlight;
        gate->cv.notify_all();
    }
}

void RankingMeasure::ProcessJob(const MeasureJob& job) {
    // Resultado para observadores externos (API de control): se completa en
    // CADA salida. /arm necesita el numero de esferas, no un ok vacio.
    MeasureResult result;
    result.jobId = job.jobId;
    result.camera = job.camera;
    result.punto = job.punto;
    result.probe = job.probe;

    // Blindaje del indice del array de clips (Enqueue ya valida).
    if (job.camera < 0 || job.camera >= kMaxCameras) {
        Logger::Warning("RankingMeasure: trabajo con camara " + std::to_string(job.camera) +
                        " fuera de rango, se descarta");
        m_droppedCount++;
        result.motivo = "camara fuera de rango";
        ReportResult(result);
        return;
    }

    // Punto 10 = "finalizar": no mide nada nuevo, consolida los dos resultados de
    // llegada (punto 8 camara 1, punto 9 camara 2) y publica el definitivo.
    if (job.punto == 10) {
        ConsolidarLlegada(job.requestedEpochMs);
        result.motivo = "finalizado";
        ReportResult(result);
        return;
    }

    const int64_t t0 = job.requestedEpochMs;
    const int64_t burstEnd = t0 + job.burstMs;

    if (job.punto >= 0 && job.punto <= 10) {
        std::lock_guard<std::mutex> lock(m_llegadaMutex);
        m_tPunto[job.punto] = t0;
    }

    // El clip de ESTA camara se cierra en CUALQUIER salida de esta funcion (la
    // guardia llama a ClipStop). ClipStop toma el lock por su cuenta: la guardia
    // nunca corre con m_clipMutex tomado.
    struct ClipGuard {
        RankingMeasure* self;
        int camara;
        ~ClipGuard() { if (self) self->ClipStop(camara); }
    } clipGuard{this, job.camera};

    // VENTANA FIJA: si el guion manda burst == max (por ejemplo 7000/7000), la
    // ventana no se busca: es [disparo, disparo + burst]. La pista es determinista
    // (la esfera entra a la ROI en el segundo 8 y sale en el 14), asi que esperar
    // un evento solo agrega error y demora. El calculo en si son milisegundos.
    if (job.maxMs == job.burstMs && job.burstMs > 0 && job.punto != 8) {
        m_puntoActual = job.punto;   // para que VectorDe use el vector del punto
        const int64_t finFijo = t0 + job.burstMs;
        Logger::Info("RankingMeasure: punto=" + std::to_string(job.punto) +
                     " ventana fija de " + std::to_string(job.burstMs) + " ms");
        // CIERRE POR GRUPO COMPLETO: la toma abre con la primera esfera (que puede venir
        // muy adelantada) y no se cierra por reloj sino cuando el conteo llega a su
        // maximo y se mantiene ~1 s. Asi el grupo que entra detras queda dentro de la
        // misma toma. El techo de burstMs queda como respaldo.
        int maxEnZona = 0;
        int estable = 0;
        int64_t finReal = finFijo;
        while (!m_stopRequested.load() && NowEpochMs() < finFijo) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::vector<FrameSample> ult;
            SnapshotHistory(job.camera, NowEpochMs() - 400, NowEpochMs(), ult);
            if (ult.empty()) continue;
            const int cuenta = EnZonaCuenta(job.camera, job.punto, ult.back());
            if (cuenta > maxEnZona) {
                maxEnZona = cuenta;
                estable = 0;
            } else if (cuenta >= maxEnZona && maxEnZona >= 3) {
                if (++estable >= 30) {          // 1 s con el grupo completo
                    finReal = NowEpochMs();
                    Logger::Info("RankingMeasure: punto=" + std::to_string(job.punto) +
                                 " cierra con el grupo completo (" +
                                 std::to_string(maxEnZona) + " esferas)");
                    break;
                }
            } else {
                estable = 0;
            }
        }
        std::vector<FrameSample> muestras;
        SnapshotHistory(job.camera, t0, (finReal < finFijo ? finReal : finFijo), muestras);
        std::vector<int> pos;
        int64_t tUlt = 0;

        // --- ORDEN POR RAYA DE CONTEO -------------------------------------------
        // Si el punto tiene raya (CAM<c>_P<p>_LINEA), el orden es el orden en que las
        // esferas la cruzan: la que cruza primero queda primera. No se proyecta nada.
        {
            const auto itl = m_lineasPunto.find((job.camera + 1) * 100 + job.punto);
            if (itl != m_lineasPunto.end() && itl->second.ok) {
                const LineaConteo& L = itl->second;
                struct Pista { double x, y; int lado; int64_t tCruce; int votes[16]; int frames; };
                std::vector<Pista> pistas;
                auto lado = [&L](double nx, double ny) {
                    const double s = (L.x2 - L.x1) * (ny - L.y1) - (L.y2 - L.y1) * (nx - L.x1);
                    return (s >= 0.0) ? 1 : -1;
                };
                for (const auto& s : muestras) {
                    if (s.enBarrido) continue;
                    for (const auto& d : s.dets) {
                        const double nx = d.x / 1920.0, ny = d.y / 1080.0;
                        if (!EnZona(job.camera, job.punto, nx, ny)) continue;
                        const int ld = lado(nx, ny);
                        const double diam = (std::max)(static_cast<double>(d.w), static_cast<double>(d.h));
                        const double puerta = (std::max)(30.0, 2.5 * diam);
                        int mejor = -1;
                        double mejorD = puerta;
                        for (size_t k = 0; k < pistas.size(); ++k) {
                            const double du = (d.x - pistas[k].x), dv = (d.y - pistas[k].y);
                            const double dist = std::sqrt(du * du + dv * dv);
                            if (dist < mejorD) { mejorD = dist; mejor = static_cast<int>(k); }
                        }
                        if (mejor < 0) {
                            Pista np{};
                            np.x = d.x; np.y = d.y; np.lado = ld; np.tCruce = 0; np.frames = 0;
                            for (int k = 0; k < 16; ++k) np.votes[k] = 0;
                            pistas.push_back(np);
                            mejor = static_cast<int>(pistas.size()) - 1;
                        }
                        Pista& p = pistas[static_cast<size_t>(mejor)];
                        if (p.tCruce == 0 && ld != p.lado) p.tCruce = s.epochMs;   // cruzo la raya
                        p.x = d.x; p.y = d.y; p.lado = ld;
                        if (d.ballID >= 0 && d.ballID < 16) p.votes[d.ballID] += 1;
                        p.frames += 1;
                    }
                }
                struct Cruce { int64_t t; int clase; int frames; };
                std::vector<Cruce> cruces;
                for (const auto& p : pistas) {
                    if (p.tCruce == 0 || p.frames < 5) continue;
                    int mejorC = -1, mejorV = 0;
                    for (int k = 0; k < 16; ++k) {
                        if (p.votes[k] > mejorV) { mejorV = p.votes[k]; mejorC = k; }
                    }
                    if (mejorC >= 0) cruces.push_back({p.tCruce, mejorC, p.frames});
                }
                std::sort(cruces.begin(), cruces.end(), [](const Cruce& a, const Cruce& b) {
                    return a.t < b.t;
                });
                Logger::Info("RankingMeasure: punto=" + std::to_string(job.punto) +
                             " raya de conteo: " + std::to_string(cruces.size()) + " cruces");
                if (!cruces.empty()) {
                    std::vector<int> porCruce;
                    std::vector<bool> visto(16, false);
                    for (const auto& cr : cruces) {
                        if (visto[static_cast<size_t>(cr.clase)]) continue;
                        visto[static_cast<size_t>(cr.clase)] = true;
                        porCruce.push_back(cr.clase + 1);   // clase -> ballID
                    }
                    if (m_publisher && !porCruce.empty()) {
                        m_publisher->PublishMeasurePoint(job.punto, tUlt, porCruce);
                    }
                    return;
                }
            }
        }

        // LECTURA DEL MEJOR CUADRO: se elige el cuadro con mas esferas dentro de la
        // zona (desempate por confianza media), se ordenan sus cajas por el avance
        // sobre el vector y ESA es la lista. Sin votar entre cuadros: votar entre 150
        // cuadros mezclaba posiciones de momentos distintos y daba una lista que no
        // era la de ningun cuadro (medido 12-nov-2026: publicado 4,2,?,8,6,1,3,7,5,?
        // contra 0,2,9,9,6,3,8,7,4,5 que es el orden real del cuadro).
        // Arco simple: proyeccion sobre la polilinea del vector SIN la pared de ROI.
        // AvanceEn() devuelve -1 cuando la pared rechaza todos los tramos, y con una
        // zona que sigue la pista en curva eso pasa siempre: la recta entre la esfera
        // y el tramo se sale del corredor. Medido 12-nov-2026: las diez esferas con
        // avance -1 y por lo tanto el orden salia en el orden en que vinieran.
        auto arcoSimple = [&](int64_t epochMs, double px, double py) {
            VectorTramo t = VectorDe(job.camera, epochMs);
            // Modo banco (sin carrera): no hay bloque de tiempo, asi que VectorDe
            // no encuentra nada. Respaldo: el vector de esa camara, el que tenga mas
            // puntos. Sin esto el avance sale -1 y no ordena nada (medido 12-nov).
            if ((!t.ok || t.pts.size() < 2) && job.camera >= 0 && job.camera < 4) {
                for (const auto& cand : m_vectores[job.camera]) {
                    if (!cand.ok || cand.pts.size() < 2) continue;
                    if (!t.ok || cand.pts.size() > t.pts.size()) t = cand;
                }
            }
            if (!t.ok || t.pts.size() < 2) return -1.0;
            double mejorD = 1e18;
            double mejorA = -1.0;
            for (size_t k = 1; k < t.pts.size(); ++k) {
                const double x1 = t.pts[k - 1].first, y1 = t.pts[k - 1].second;
                const double x2 = t.pts[k].first, y2 = t.pts[k].second;
                const double dx = x2 - x1, dy = y2 - y1;
                const double l2 = dx * dx + dy * dy;
                if (l2 <= 1e-12) continue;
                double u = ((px - x1) * dx + (py - y1) * dy) / l2;
                if (u < 0.0) u = 0.0;
                if (u > 1.0) u = 1.0;
                const double qx = x1 + u * dx, qy = y1 + u * dy;
                const double d2 = (px - qx) * (px - qx) + (py - qy) * (py - qy);
                if (d2 < mejorD) {
                    mejorD = d2;
                    mejorA = t.arco[k - 1] + u * (t.arco[k] - t.arco[k - 1]);
                }
            }
            return mejorA;
        };

        double mejorPuntaje = -1.0;
        std::vector<int> mejorLista;
        int cuadrosUtiles = 0;
        for (const auto& s : muestras) {
            if (s.enBarrido) continue;
            std::vector<std::pair<double, int>> orden;
            orden.reserve(s.dets.size());
            double conf = 0.0;
            for (size_t k = 0; k < s.dets.size(); ++k) {
                const FrameDetection& d = s.dets[k];
                if (!m_bancoSinRoi.load() && !EnZona(job.camera, job.punto, d.x / 1920.0, d.y / 1080.0)) continue;
                orden.emplace_back(arcoSimple(s.epochMs, d.x / 1920.0, d.y / 1080.0),
                                   static_cast<int>(k));
                conf += d.conf;
            }
            if (static_cast<int>(orden.size()) < 6) continue;
            ++cuadrosUtiles;
            const double puntaje = static_cast<double>(orden.size()) * 1000.0 +
                                   conf / (std::max)(1.0, static_cast<double>(orden.size()));
            if (puntaje <= mejorPuntaje) continue;
            std::sort(orden.begin(), orden.end(), [](const auto& a, const auto& b) {
                return a.first > b.first;   // mas avance = mejor puesto
            });
            mejorLista.clear();
            std::vector<float> confDe;
            for (const auto& par : orden) {
                const FrameDetection& d = s.dets[static_cast<size_t>(par.second)];
                const int c = d.ballID;
                if (c >= 0 && c < static_cast<int>(kTargetSpheres)) {
                    mejorLista.push_back(c);
                    confDe.push_back(d.conf);
                }
            }
            // Si el cuadro trae 10 cajas y una clase se repite mientras otra falta,
            // una de las repetidas ES la que falta (hay una sola esfera de cada una).
            // Medido 12-nov-2026: el cuadro traia dos B9 y ningun B1; la cuarta esfera
            // (amarilla) es la 1, y la lista salia con una clase de menos.
            if (mejorLista.size() == static_cast<size_t>(kTargetSpheres)) {
                int cuenta[kTargetSpheres] = {0};
                for (int c : mejorLista) cuenta[c] += 1;
                for (int c = 0; c < static_cast<int>(kTargetSpheres); ++c) {
                    if (cuenta[c] != 1) continue;
                    // falta esta clase: se la lleva la repetida de menor confianza
                    int rep = -1;
                    float peor = 2.0f;
                    for (size_t k = 0; k < mejorLista.size(); ++k) {
                        const int cc = mejorLista[k];
                        if (cuenta[cc] < 2) continue;
                        if (confDe[k] < peor) { peor = confDe[k]; rep = static_cast<int>(k); }
                    }
                    if (rep >= 0 && cuenta[mejorLista[static_cast<size_t>(rep)]] >= 2) {
                        Logger::Info("RankingMeasure: cuadro con clase repetida " +
                                     std::to_string(mejorLista[static_cast<size_t>(rep)]) +
                                     " y sin la " + std::to_string(c) + " -> se reasigna");
                        cuenta[mejorLista[static_cast<size_t>(rep)]] -= 1;
                        mejorLista[static_cast<size_t>(rep)] = c;
                        cuenta[c] = 1;
                    }
                }
            }
            mejorPuntaje = puntaje;
            tUlt = s.epochMs;
            {
                std::ostringstream dbg;
                dbg << "RankingMeasure: cuadro elegido t=" << s.epochMs << " ->";
                for (size_t k = 0; k < orden.size(); ++k) {
                    const FrameDetection& d = s.dets[static_cast<size_t>(orden[k].second)];
                    dbg << " B" << d.ballID << "@" << static_cast<long long>(orden[k].first);
                }
                Logger::Info(dbg.str());
            }
        }
        Logger::Info("RankingMeasure: punto=" + std::to_string(job.punto) +
                     " lectura de cuadro: " + std::to_string(cuadrosUtiles) +
                     " cuadros utiles, mejor con " + std::to_string(mejorLista.size()) + " esferas");
        // Una clase repetida no puede ocupar dos puestos: se descarta la segunda.
        std::vector<bool> usado(static_cast<size_t>(kTargetSpheres), false);
        for (int c : mejorLista) {
            if (c < 0 || c >= static_cast<int>(kTargetSpheres)) continue;
            if (usado[static_cast<size_t>(c)]) continue;
            usado[static_cast<size_t>(c)] = true;
            pos.push_back(c + 1);   // clase (0..9) -> ballID (1..10)
        }
        const bool ok = !pos.empty();
        Logger::Info("RankingMeasure: punto=" + std::to_string(job.punto) + " camara=" +
                     std::to_string(job.camera) + " ventana fija: " +
                     std::to_string(muestras.size()) + " cuadros -> " +
                     std::to_string(pos.size()) + " posiciones" +
                     (ok ? "" : " (no resolvio)"));
        if (ok && !pos.empty() && pos.size() <= static_cast<size_t>(kTargetSpheres)) {
            if (m_publisher) m_publisher->PublishMeasurePoint(job.punto, tUlt, pos);
        }
        return;
    }

    // 1. VENTANA POR EVENTO OBSERVADO.
    //    No se mide "desde el reloj y durante X": se espera el evento que marca el
    //    inicio (>=3 esferas en ROI para los puntos intermedios, las 10 para la
    //    llegada), y se cierra cuando el fenomeno termina (el pasaje sale del
    //    cuadro / el tablero queda quieto) o al vencer el plazo, lo primero.
    // Punto 8 = llegada por la camara 1. Punto 9 = la MISMA llegada vista por la
    // camara 2 (evento paralelo al 8): dos medidas independientes del resultado.
    // El punto 7 queda como pasaje, publicando el estado parcial de esa camara.
    // Punto 8 = llegada (espera las 10 y la quietud). El resto, incluido el 7,
    // mide su pasaje: publica lo que ve en su ventana. El punto 10 ("finalizar")
    // confirma despues los resultados de 7 y 8.
    const bool esLlegada = (job.punto == 9);
    const int minDetsArranque = esLlegada ? kTargetSpheres : 3;
    // Tope duro de la toma informativa: 8 s. Con un solo hilo medidor, una toma
    // que no cierra bloquea a los puntos siguientes y esos abren cuando la camara
    // ya cambio de preset (medido 12-nov-2026: p3 abrio a +31s en vez de +10 y la
    // ROI cayo sobre otra parte de la pista). El tope mantiene la cola avanzando.
    const int64_t plazoMs = job.burstMs;      // 2 s intermedios · 20 s llegada
    const int64_t topeAbs = t0 + job.maxMs;   // techo duro por si el evento no llega

    int64_t inicioEvento = 0;
    // Racha de kCuadrosArranque cuadros CONSECUTIVOS con el minimo de esferas.
    // Los sondeos van cada 10 ms y los cuadros llegan cada ~33 ms: se cuentan
    // muestras distintas por epoch, no sondeos, o el mismo cuadro contaria 3 veces.
    int racha = 0;
    int64_t rachaIni = 0;
    int64_t ultimoEpoch = 0;
    int sondeos = 0;
    while (!m_stopRequested.load() && NowEpochMs() < topeAbs) {
        // Camara moviendose: no se abre nada todavia.
        if (NowEpochMs() < m_movHasta[job.camera].load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        std::vector<FrameSample> ult;
        SnapshotHistory(job.camera, NowEpochMs() - 600, NowEpochMs(), ult);
        if (!ult.empty() && ult.back().epochMs != ultimoEpoch) {
            ultimoEpoch = ult.back().epochMs;
            if (EnZonaCuenta(job.camera, job.punto, ult.back()) >= minDetsArranque) {
                if (racha == 0) {
                    rachaIni = ultimoEpoch;
                }
                ++racha;
                if (racha >= kCuadrosArranque) {
                    inicioEvento = rachaIni;   // la ventana incluye la racha completa
                    break;
                }
            } else {
                racha = 0;
            }
        }
        // Cada 200 ms: si el pasaje ya ocurrio y estamos tarde (esperamos en la cola),
        // se toma la ultima racha COMPLETA del historial. Es solo leer.
        if (inicioEvento == 0 && (++sondeos % 20) == 0) {
            std::vector<FrameSample> hist;
            SnapshotHistory(job.camera, NowEpochMs() - kBusquedaAtrasMs, NowEpochMs(), hist);
            int r = 0;
            int64_t ini = 0;
            for (size_t i = hist.size(); i-- > 0;) {
                if (EnZonaCuenta(job.camera, job.punto, hist[i]) >= minDetsArranque) {
                    if (r == 0) {
                        ini = hist[i].epochMs;
                    }
                    ++r;
                } else {
                    if (r >= kCuadrosArranque &&
                        ini >= m_movHasta[job.camera].load(std::memory_order_relaxed)) {
                        inicioEvento = ini;   // pasaje completo: entro y salio
                        break;
                    }
                    r = 0;
                }
            }
            if (inicioEvento != 0) {
                Logger::Info("RankingMeasure: punto=" + std::to_string(job.punto) +
                             " evento encontrado hacia atras en el historial");
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (inicioEvento == 0) {
        // Antes caia a t0 y medía 25 s de nada: con el medidor de un solo hilo eso
        // encolaba a todos los puntos siguientes (medido 12-nov-2026: p4..p8
        // arrancaron juntos a +56s con "toma desde +0.0s"). Ahora se descarta,
        // salvo el punto 1, que mide las 10 arriba antes de abrir la compuerta.
        if (job.punto == 1) {
            inicioEvento = t0;
        } else {
            Logger::Warning("RankingMeasure: punto=" + std::to_string(job.punto) +
                            " camara=" + std::to_string(job.camera) +
                            " no llego el grupo al tramo en la espera, se descarta");
            m_droppedCount++;
            result.spheres = 0;
            result.motivo = "no llego el grupo al tramo";
            ReportResult(result);
            return;
        }
    }

    // Traza de inicio: hasta ahora el log solo mostraba el final de la toma, asi que
    // no se veia CUANDO empezaba a medir cada punto (pedido del dueno, 12-nov-2026).
    {
        const double desdeT0 = static_cast<double>(inicioEvento - t0) / 1000.0;
        const double arranque = static_cast<double>(NowEpochMs() - t0) / 1000.0;
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      " empieza a medir (toma desde +%.1fs, escuchando desde +%.1fs, plazo %ds)",
                      desdeT0, arranque, static_cast<int>(plazoMs / 1000));
        Logger::Info("RankingMeasure: punto=" + std::to_string(job.punto) +
                     " camara=" + std::to_string(job.camera) + buf);
    }

    // Cierre: el pasaje termina (ultimo cuadro sin esferas) o el tablero se queda
    // quieto; en ambos casos tambien vence el plazo.
    // Informativas: 8 s de tope. La llegada (punto 8) conserva su plazo largo porque
    // tiene que esperar a que las diez se detengan en el tablero.
    const int64_t topeToma = esLlegada ? plazoMs : (plazoMs > 8000 ? 8000 : plazoMs);
    const int64_t cierreTope = inicioEvento + topeToma;
    int vaciosZona = 0;
    int bajosZona = 0;      // cuadros seguidos con <=2 en zona despues del pico
    bool noPaso = false;    // la zona nunca se lleno: el grupo no vino
    int maxVistoZona = 0;   // para no cerrar por quietud antes de que pase el grupo
    while (!m_stopRequested.load() && NowEpochMs() < cierreTope) {
        std::vector<FrameSample> ult;
        SnapshotHistory(job.camera, NowEpochMs() - 1800, NowEpochMs(), ult);
        if (ult.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        if (!esLlegada) {
            // El pasaje termina cuando la ZONA queda vacia 10 cuadros seguidos
            // (0,33 s): un cuadro suelto sin detecciones no cierra la toma.
            const int cuentaZona = EnZonaCuenta(job.camera, job.punto, ult.back());
            if (cuentaZona == 0) {
                if (++vaciosZona >= 10) break;
            } else {
                vaciosZona = 0;
                maxVistoZona = (std::max)(maxVistoZona, cuentaZona);
                // (a) El grupo ya paso: hubo pico (>=6) y ahora quedan <=2 en la
                // zona. Evita quedarse 25 s con 4-5 esferas que nunca se vacian.
                if (maxVistoZona >= 6 && cuentaZona <= 2) {
                    if (++bajosZona >= 10) {
                        Logger::Info("RankingMeasure: punto=" + std::to_string(job.punto) +
                                     " cierra: el grupo ya paso (quedan " +
                                     std::to_string(cuentaZona) + " en la zona)");
                        break;
                    }
                } else {
                    bajosZona = 0;
                }
                // (c) Nunca llego el grupo: 8 s sin pasar de 3 esferas en la zona.
                if (maxVistoZona < 3 && (NowEpochMs() - inicioEvento) > 8000) {
                    noPaso = true;
                    break;
                }
                // Cierre por quietud: si el grupo se detuvo DENTRO de la zona, la
                // toma ya tiene todo lo que va a tener. Antes se esperaba al techo
                // de 25 s con la zona ocupada, y eso retrasaba a los puntos
                // siguientes en la cola del medidor (un solo hilo).
                const bool yaHuboPasaje = (maxVistoZona >= 6) &&
                                          (NowEpochMs() - inicioEvento > 3000);
                if (yaHuboPasaje && QuietoEnZona(job.camera, job.punto, ult, 45, 2.0)) {   // 1,5 s
                    Logger::Info("RankingMeasure: punto=" + std::to_string(job.punto) +
                                 " cierra por quietud (el grupo se detuvo en la zona)");
                    break;
                }
            }
        } else {
            if (EnZonaCuenta(job.camera, job.punto, ult.back()) >= kTargetSpheres &&
                EstableQuieto(ult, 30, 2.0)) break;      // 1 s con las 10 quietas
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const int64_t ahoraCierre = NowEpochMs();
    const int64_t ventanaFin = (ahoraCierre < cierreTope) ? ahoraCierre : cierreTope;

    // La ventana ya cerro: si la camara no mando ni un cuadro, no hay que
    // dejar la marca puesta (abriria el clip de un punto posterior).
    {
        std::lock_guard<std::mutex> lock(m_clipMutex);
        if (m_ventanaPunto[job.camera] == job.punto) {
            m_ventanaPunto[job.camera] = -1;
        }
    }

    if (noPaso) {
        Logger::Warning("RankingMeasure: punto=" + std::to_string(job.punto) +
                        " el grupo no paso por el tramo en 8 s, se descarta");
        m_droppedCount++;
        return;
    }

    // 2. Tomar lo que el pipeline vio en la ventana.
    std::vector<FrameSample> samples;
    SnapshotHistory(job.camera, inicioEvento, ventanaFin, samples);
    if (MaxDetections(samples) < kTargetSpheres && job.maxMs > plazoMs) {
        const int64_t hardEnd = inicioEvento + job.maxMs;
        while (!m_stopRequested.load() && NowEpochMs() < hardEnd) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        SnapshotHistory(job.camera, t0, hardEnd, samples);
    }
    if (samples.empty()) {
        Logger::Warning("RankingMeasure: punto=" + std::to_string(job.punto) +
                        " sin cuadros en la ventana (camara " +
                        std::to_string(job.camera) + "), se descarta");
        m_droppedCount++;
        result.motivo = "sin cuadros en la ventana (camara " +
                        std::to_string(job.camera) + ")";
        ReportResult(result);
        return;
    }
    result.maxDetections = MaxDetections(samples);

    // Diagnostico: cuantos cuadros y cajas llegaron al medidor para este punto.
    {
        size_t cajasTotal = 0;
        for (const auto& f : samples) cajasTotal += f.dets.size();
        Logger::Info("RankingMeasure: punto=" + std::to_string(job.punto) +
                     " camara=" + std::to_string(job.camera) +
                     " muestras=" + std::to_string(samples.size()) +
                     " cajas=" + std::to_string(cajasTotal) +
                     " maxDet=" + std::to_string(result.maxDetections));
    }

    std::vector<int> posiciones;
    int64_t resolvedEpochMs = 0;
    if (!ResolvePositions(job.camera, job.punto, samples, posiciones, resolvedEpochMs)) {
        Logger::Warning("RankingMeasure: punto=" + std::to_string(job.punto) +
                        " sin detecciones en la rafaga, se descarta");
        m_droppedCount++;
        result.spheres = 0;
        result.motivo = "sin detecciones en la rafaga";
        ReportResult(result);
        return;
    }
    result.spheres = static_cast<int>(posiciones.size());
    result.epochMs = resolvedEpochMs;

    // Se guarda el ultimo resultado de cada punto: el "finalizar" usa el 7 (camara 2)
    // y el 8 (camara 1), que son las dos vistas del tablero.
    if (job.punto >= 0 && job.punto <= 10) {
        std::lock_guard<std::mutex> lock(m_llegadaMutex);
        m_llegadaPunto[job.punto] = posiciones;
    }

    int barridos = 0;
    for (const auto& s : samples) {
        if (s.enBarrido) {
            ++barridos;
        }
    }
    {
        std::ostringstream oss;
        oss << "RankingMeasure: punto=" << job.punto
            << " camara=" << job.camera
            << " frames=" << samples.size()
            << " barrido=" << barridos
            << " maxDetecciones=" << MaxDetections(samples)
            << " t=" << resolvedEpochMs
            << " posiciones=[";
        for (size_t i = 0; i < posiciones.size(); ++i) {
            if (i > 0) oss << ",";
            oss << posiciones[i];
        }
        oss << "]";
        Logger::Info(oss.str());
    }

    // Se publica lo que REALMENTE se observa: si faltan esferas, esas canicas van
    // con puesto 0, que el receptor dibuja como "indeterminado" (asi la llegada se
    // ve aunque falte una). Lo que NO se publica es con MAS de 10: identidades de
    // sobra entre los cuadros de la rafaga producen puestos que no existen
    // (medido 3-nov-2026: 13 posiciones -> {"id":0,"puesto":11}).
    if (static_cast<int>(posiciones.size()) > kTargetSpheres || posiciones.empty()) {
        Logger::Warning("RankingMeasure: punto=" + std::to_string(job.punto) +
                        " resuelto con " + std::to_string(posiciones.size()) +
                        " posiciones (se publican hasta " + std::to_string(kTargetSpheres) +
                        " y al menos 1), no se publica");
        m_droppedCount++;
        result.motivo = std::to_string(posiciones.size()) + " esferas resueltas";
        ReportResult(result);
        return;
    }
    if (static_cast<int>(posiciones.size()) < kTargetSpheres) {
        Logger::Info("RankingMeasure: punto=" + std::to_string(job.punto) + " se publica incompleto: " +
                     std::to_string(posiciones.size()) + " de " + std::to_string(kTargetSpheres) +
                     " (las que faltan van con puesto 0)");
    }

    // El punto 8 es una MEDICION mas: publica incompleto igual que los intermedios.
    // Lo que se exige exacto es el FINAL que se calcula con 8 + 7 (punto 10).

    // Una sonda de la API de control (/arm) NO publica: solo verifica.
    if (!job.probe) {
        if (m_publisher && m_publisher->PublishMeasurePoint(job.punto, resolvedEpochMs, posiciones)) {
            m_measureCount++;
            result.published = true;
            // Copia thread-safe para GET /status y el final de la carrera.
            {
                std::lock_guard<std::mutex> lock(m_lastRankingMutex);
                m_lastRanking = posiciones;
                m_lastRankingPunto = job.punto;
                m_lastRankingEpochMs = resolvedEpochMs;
            }
        } else {
            m_droppedCount++;
            result.motivo = "no se pudo publicar el punto (publisher)";
        }
    } else {
        Logger::Info("RankingMeasure: sonda camara=" + std::to_string(job.camera) +
                     " con " + std::to_string(posiciones.size()) +
                     " esferas (no se publica)");
    }

    ReportResult(result);
}

// ----------------------------------------------------------------------------
// Ingesta y resolucion
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// El pipeline nos pasa lo que ya infirio
// ----------------------------------------------------------------------------

void RankingMeasure::OnDetections(int camera, int64_t epochMs,
                                  const std::vector<BallDetection>& dets,
                                  const void* deviceFrame, size_t frameBytes,
                                  unsigned int width, unsigned int height) {
    if (camera < 0 || camera >= kMaxCameras) {
        return;
    }

    // Corte automatico de la carrera: a los race_seconds desde la compuerta
    // (RaceStart) la carrera se cierra sola y se vuelca, sin esperar al fin del
    // guion ni al cierre del programa. Se dispara UNA SOLA vez: el hilo de
    // camara que gana el exchange es el unico que vuelca; los otros (y los
    // cuadros siguientes de este) ven m_raceActiva=false y no hacen nada.
    if (m_config.saveRace && m_raceActiva.load() &&
        epochMs >= m_raceInicioMs.load() + static_cast<int64_t>(m_config.raceSeconds) * 1000) {
        if (m_raceActiva.exchange(false)) {
            Logger::Info("RankingMeasure: carrera: cumplidos " +
                         std::to_string(m_config.raceSeconds) +
                         " s desde la compuerta; se cierra y se vuelca");
            DumpRace();
        }
    }

    // Carrera completa en RAM (save_race). El if va ANTES de cualquier
    // cudaMemcpy y antes de tocar el anillo: con save_race=false (default) este
    // bloque no reserva ni un byte ni copia ni un cuadro. Y con la carrera sin
    // arrancar (m_raceActiva=false, o sea antes de la compuerta) tampoco copia:
    // asi el anillo no se llena con cuadros previos a la carrera.
    if (m_config.saveRace && m_raceActiva.load() &&
        deviceFrame != nullptr && frameBytes > 0) {
        // Las detecciones van solo para quemar las cajas en el MP4 de la carrera
        // (draw_boxes); con draw_boxes=false RacePush no guarda ni una caja.
        RacePush(camera, deviceFrame, frameBytes, epochMs, width, height, dets);
    }

    // Evidencia. El estado del clip es POR CAMARA: este cuadro solo puede caer en
    // el clip de `camera`. Si esa camara tiene una ventana pedida, el clip se abre
    // con ESTE cuadro (asi las dimensiones son las reales) y se empuja despues.
    {
        std::lock_guard<std::mutex> lock(m_clipMutex);
        if (m_ventanaPunto[camera] >= 0) {
            if (m_clips[camera].recording) {
                Logger::Warning("RankingMeasure: habia un clip sin cerrar en la camara " +
                                std::to_string(camera) + " (medicion descartada por cola "
                                "llena); se descarta su evidencia");
            }
            ClipStartLocked(camera, m_ventanaPunto[camera], width, height, epochMs);
            m_ventanaPunto[camera] = -1;
        }
    }
    // ClipPush toma el lock y solo empuja si ESA camara esta grabando: las otras
    // tres camaras no aportan ni un byte a este clip.
    if (deviceFrame && frameBytes > 0) {
        ClipPush(camera, deviceFrame, frameBytes);
    }

    if (dets.empty()) {
        // El cuadro entra igual al volcado de inferencia, con n=0: es la unica
        // forma de saber cuantos cuadros vio el pipeline y cuales salieron sin
        // deteccion. Cuesta un push de un vector vacio.
        if (m_config.saveInference) {
            MuestraCamara vacia;
            vacia.epochMs = epochMs;
            InfPush(camera, vacia);
        }
        return;
    }

    // Dedupe por distancia dentro del cuadro: el NMS del motor es por clase, asi
    // que la misma esfera puede venir dos veces con clases distintas. Se queda
    // la de mayor confianza (medido 3-nov: dos cajas para la esfera de x=425).
    // El umbral es 0,60 diametros: las esferas vecinas del ranking estan a ~0,8,
    // asi que 1,2 fusionaba dos esferas reales en una.
    const float W = static_cast<float>(width);
    const float H = static_cast<float>(height);

    std::vector<BallDetection> ordenadas = dets;
    std::stable_sort(ordenadas.begin(), ordenadas.end(),
                     [](const BallDetection& a, const BallDetection& b) {
                         return a.confidence > b.confidence;
                     });

    std::vector<BallDetection> unicas;
    unicas.reserve(ordenadas.size());
    for (const auto& det : ordenadas) {
        bool repetida = false;
        for (const auto& k : unicas) {
            const float dx = (det.x - k.x) * W;
            const float dy = (det.y - k.y) * H;
            const float diam = (std::max)(k.width * W, k.height * H);
            if (diam <= 0.0f) {
                continue;
            }
            if (std::sqrt(dx * dx + dy * dy) < 0.60f * diam) {
                repetida = true;
                break;
            }
        }
        if (!repetida) {
            unicas.push_back(det);
        }
    }

    // El filtro por ROI YA NO se hace aca: se guardan todas las cajas del cuadro y
    // cada punto recorta a SU zona al medir (EnZona). Asi el filtro no depende del
    // reloj ni de que la camara ya se haya movido.

    MuestraCamara muestra;
    muestra.epochMs = epochMs;
    muestra.dets.reserve(unicas.size());
    for (const auto& det : unicas) {
        FrameDetection d;
        d.x = det.x * W;
        d.y = det.y * H;
        d.w = det.width * W;
        d.h = det.height * H;
        d.ballID = det.ballID;
        d.conf = det.confidence;
        muestra.dets.push_back(d);
    }

    // Volcado de inferencia: copia las cajas de este cuadro al anillo de la
    // carrera (unos 300 B, sin E/S). Va ANTES del move para no quedarse con la
    // muestra vacia. Con save_inference apagado cuesta una lectura atomica.
    InfPush(camera, muestra);

    std::lock_guard<std::mutex> lock(m_historiaMutex);
    auto& cola = m_historia[camera];
    cola.push_back(std::move(muestra));
    while (cola.size() > kHistoriaPorCamara) {
        cola.pop_front();
    }
}

void RankingMeasure::SnapshotHistory(int camera, int64_t fromMs, int64_t toMs,
                                     std::vector<FrameSample>& out) {
    out.clear();
    if (camera < 0 || camera >= kMaxCameras) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_historiaMutex);
    for (const auto& m : m_historia[camera]) {
        if (m.epochMs < fromMs || m.epochMs > toMs) {
            continue;
        }
        FrameSample s;
        s.epochMs = m.epochMs;
        s.dets = m.dets;
        out.push_back(std::move(s));
    }
}

// ---------------------------------------------------------------------------
// Consolida las dos medidas de llegada (punto 8 camara 1, punto 9 camara 2).
// Las dos camaras pueden ver el mismo tablero en sentidos opuestos, asi que se
// compara A contra B y A contra B invertido y se queda con el sentido que mas
// coincide. Despues publica el resultado: si los dos traen las 10 clases, gana el
// que las tenga todas distintas.
void RankingMeasure::ConsolidarLlegada(int64_t epochMs) {
    int64_t tP8 = 0, tP7 = 0;
    std::vector<int> a, b;
    {
        std::lock_guard<std::mutex> lock(m_llegadaMutex);
        a = m_llegadaPunto[8];
        b = m_llegadaPunto[7];
        tP8 = m_tPunto[9];
        tP7 = m_tPunto[8];
    }

    // Lo que importa: reconstruir el final con TODOS los cuadros de cada camara
    // desde su punto hasta el cierre, no con lo que se publico en el punto.
    // Camara 1 (indice 0) desde el punto 8; camara 2 (indice 1) desde el punto 7.
    const int64_t ahora = NowEpochMs();
    auto reconstruir = [&](int cam, int64_t desde) {
        std::vector<int> pos;
        if (desde <= 0) return pos;
        std::vector<FrameSample> muestras;
        SnapshotHistory(cam, desde, ahora, muestras);
        int64_t t = 0;
        if (!muestras.empty() && ResolvePositions(cam, (cam == 0) ? 8 : 7, muestras, pos, t)) {
            Logger::Info("RankingMeasure: finalizar reconstruyo camara " + std::to_string(cam + 1) +
                         " con " + std::to_string(muestras.size()) + " cuadros -> " +
                         std::to_string(pos.size()) + " posiciones");
        } else {
            Logger::Info("RankingMeasure: finalizar NO pudo reconstruir camara " +
                         std::to_string(cam + 1) + " (" + std::to_string(muestras.size()) + " cuadros)");
            pos.clear();
        }
        return pos;
    };
    const std::vector<int> ra = reconstruir(0, tP8);
    const std::vector<int> rb = reconstruir(1, tP7);
    {
        auto listar = [](const std::vector<int>& v) {
            std::ostringstream o;
            o << "[";
            for (size_t i = 0; i < v.size(); ++i) { if (i) o << ","; o << v[i]; }
            o << "]";
            return o.str();
        };
        Logger::Info("RankingMeasure: finalizar camara 1 -> " + listar(ra));
        Logger::Info("RankingMeasure: finalizar camara 2 -> " + listar(rb));
    }
    if (!ra.empty()) a = ra;
    if (!rb.empty()) b = rb;
    auto distintas = [](const std::vector<int>& v) {
        std::set<int> s(v.begin(), v.end());
        return static_cast<int>(s.size());
    };
    auto coinciden = [](const std::vector<int>& x, const std::vector<int>& y) {
        const size_t n = (x.size() < y.size()) ? x.size() : y.size();
        int c = 0;
        for (size_t i = 0; i < n; ++i) if (x[i] == y[i]) ++c;
        return c;
    };

    std::vector<int> elegido;
    std::string nota;
    if (a.empty() && b.empty()) {
        Logger::Warning("RankingMeasure: finalizar sin resultados de llegada");
        return;
    }
    if (a.empty() || b.empty()) {
        elegido = a.empty() ? b : a;
        nota = "solo una camara";
    } else {
        // NO se invierte ninguna camara: cada una publica en su propio sentido.
        // La 1 y la 2 miran desde lados distintos; invertir a ciegas elegia mal.
        const int cb = coinciden(a, b);
        const int cr = 0;
        const int c = cb;
        const size_t nMenor = (a.size() < b.size()) ? a.size() : b.size();
        Logger::Info("RankingMeasure: finalizar, coinciden " + std::to_string(c) + " de " +
                     std::to_string(nMenor) + " posiciones" +
                     (nota.empty() ? "" : " (" + nota + ")"));
        if (distintas(b) > distintas(a)) { elegido = b; nota += " · gana camara 2"; }
        else { elegido = a; }
    }
    if (elegido.empty()) return;
    Logger::Info("RankingMeasure: finalizar -> " + std::to_string(elegido.size()) +
                 " posiciones" + (nota.empty() ? "" : " (" + nota + ")"));
    if (static_cast<int>(elegido.size()) != kTargetSpheres) {
        Logger::Warning("RankingMeasure: finalizar NO publica: " +
                        std::to_string(elegido.size()) + " de " +
                        std::to_string(kTargetSpheres) + " posiciones (el final se exige exacto)");
        return;
    }
    if (m_publisher) {
        m_publisher->PublishMeasurePoint(10, epochMs, elegido);
        m_measureCount++;
    }
}

// ---------------------------------------------------------------------------
// Quietud del tablero: las ultimas nFrames muestras tienen las 10 esferas y cada
// una se movio menos de maxPx (pixeles) respecto de la anterior. Se empareja cada
// caja con la mas cercana del cuadro previo (el orden del vector no es estable).
bool RankingMeasure::QuietoEnZona(int camera, int punto, const std::vector<FrameSample>& samples, int nFrames, double maxPx) const {
    if (static_cast<int>(samples.size()) < nFrames) return false;
    const size_t ini = samples.size() - static_cast<size_t>(nFrames);
    std::vector<std::vector<std::pair<double,double>>> centros(nFrames);
    for (size_t i = ini; i < samples.size(); ++i) {
        auto& c = centros[i - ini];
        for (const auto& d : samples[i].dets) {
            if (EnZona(camera, punto, d.x / 1920.0, d.y / 1080.0)) c.emplace_back(d.x, d.y);
        }
        if (c.empty()) return false;
    }
    // Tolerante al parpadeo del detector: el conteo puede variar en 1 entre cuadros
    // (10, 11, 9...) y antes eso impedia cerrar (medido 12-nov-2026: punto 6 en el
    // techo de 25 s dos carreras seguidas con el grupo parado adentro).
    size_t minN = centros.front().size(), maxN = centros.front().size();
    for (const auto& c : centros) { minN = (std::min)(minN, c.size()); maxN = (std::max)(maxN, c.size()); }
    if (minN == 0 || maxN - minN > 2) return false;
    for (size_t i = 1; i < centros.size(); ++i) {
        std::vector<double> d;
        for (const auto& p : centros[i]) {
            double mejor = 1e18;
            for (const auto& q : centros[i-1]) {
                const double dx = p.first - q.first, dy = p.second - q.second;
                const double d2 = dx*dx + dy*dy;
                if (d2 < mejor) mejor = d2;
            }
            d.push_back(std::sqrt(mejor));
        }
        if (d.empty()) return false;
        std::sort(d.begin(), d.end());
        const double mediana = d[d.size() / 2];
        if (mediana > maxPx) return false;
    }
    return true;
}

// Quietud del tablero para la LLEGADA: las ultimas nFrames muestras tienen las 10
// esferas y cada una se movio menos de maxPx (pixeles) respecto de la anterior.
bool RankingMeasure::EstableQuieto(const std::vector<FrameSample>& samples, int nFrames, double maxPx) {
    if (static_cast<int>(samples.size()) < nFrames) return false;
    const size_t ini = samples.size() - static_cast<size_t>(nFrames);
    for (size_t i = ini; i < samples.size(); ++i) {
        if (static_cast<int>(samples[i].dets.size()) < kTargetSpheres) return false;
    }
    for (size_t i = ini + 1; i < samples.size(); ++i) {
        const auto& a = samples[i - 1].dets;
        const auto& b = samples[i].dets;
        double suma = 0.0;
        int n = 0;
        for (const auto& d : b) {
            double mejor = 1e9;
            for (const auto& p : a) {
                // d.x/d.y YA vienen en pixeles: multiplicar por 1920 convertia medio
                // pixel en 960 y la llegada nunca cerraba por quietud (12-nov-2026).
                const double dx = d.x - p.x;
                const double dy = d.y - p.y;
                const double dist = std::sqrt(dx * dx + dy * dy);
                if (dist < mejor) mejor = dist;
            }
            suma += mejor;
            ++n;
        }
        if (n > 0 && (suma / n) > maxPx) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Direccion de avance por tramo, leida de env_calibracion.
// Formato: CAM1_TRACK_VECTOR_44_59=0.996,0.572;0.016,0.418
// El primer punto es donde entra el grupo y el ultimo donde termina el tramo:
// la direccion va del primero al ultimo, y "mas adelante" = mayor proyeccion.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// points.json: el punto es la unidad. Cada vista trae camara, zona, vector y raya.
// Se llenan las mismas estructuras que usa el medidor, asi la resolucion no cambia.
// ---------------------------------------------------------------------------
bool RankingMeasure::CargarPuntos() {
    const char* candidatos[] = { "C:\\vs\\points.json", "points.json", "..\\points.json" };
    std::ifstream f;
    std::string usada;
    for (const char* c : candidatos) {
        f.open(c);
        if (f.is_open()) { usada = c; break; }
    }
    if (!f.is_open()) return false;

    nlohmann::json j;
    try {
        f >> j;
    } catch (...) {
        Logger::Warning("RankingMeasure: points.json no se pudo leer");
        return false;
    }
    if (!j.contains("points")) return false;

    int nz = 0, nv = 0, nl = 0;
    for (const auto& p : j["points"]) {
        const int punto = p.value("point", 0);
        if (punto <= 0 || !p.contains("views")) continue;
        for (const auto& v : p["views"]) {
            const int cam = v.value("camera", 0);
            if (cam <= 0) continue;
            const int clave = cam * 100 + punto;

            if (v.contains("zone") && v["zone"].is_array()) {
                std::vector<std::pair<double, double>> zona;
                for (const auto& pt : v["zone"]) {
                    if (pt.is_array() && pt.size() >= 2) {
                        zona.emplace_back(pt[0].get<double>(), pt[1].get<double>());
                    }
                }
                if (zona.size() >= 3) {
                    m_zonas[clave] = zona;
                    ++nz;
                }
            }

            if (v.contains("vector") && v["vector"].is_array()) {
                VectorTramo t;
                for (const auto& pt : v["vector"]) {
                    if (pt.is_array() && pt.size() >= 2) {
                        t.pts.emplace_back(pt[0].get<double>(), pt[1].get<double>());
                    }
                }
                if (t.pts.size() >= 2) {
                    t.arco.clear();
                    t.arco.push_back(0.0);
                    for (size_t i = 1; i < t.pts.size(); ++i) {
                        const double dx = t.pts[i].first - t.pts[i - 1].first;
                        const double dy = t.pts[i].second - t.pts[i - 1].second;
                        t.arco.push_back(t.arco.back() + std::sqrt(dx * dx + dy * dy));
                    }
                    t.x0 = t.pts.front().first;
                    t.y0 = t.pts.front().second;
                    t.ok = true;
                    m_vectoresPunto[clave] = t;
                    ++nv;
                }
            }

            if (v.contains("line") && v["line"].is_array() && v["line"].size() >= 2) {
                const auto& L = v["line"];
                if (L[0].is_array() && L[1].is_array() && L[0].size() >= 2 && L[1].size() >= 2) {
                    LineaConteo lc;
                    lc.x1 = L[0][0].get<double>();
                    lc.y1 = L[0][1].get<double>();
                    lc.x2 = L[1][0].get<double>();
                    lc.y2 = L[1][1].get<double>();
                    lc.ok = true;
                    m_lineasPunto[clave] = lc;
                    ++nl;
                }
            }
        }
    }
    Logger::Info("RankingMeasure: points.json (" + usada + "): " + std::to_string(nz) + " zonas, " +
                 std::to_string(nv) + " vectores, " + std::to_string(nl) + " rayas");
    return nz > 0;
}

void RankingMeasure::LoadTrackVectors() {
    for (auto& v : m_vectores) v.clear();
    // Si existe points.json, manda el punto: ahi vienen camara, zona, vector y raya juntos.
    if (CargarPuntos()) return;
    const char* candidatos[] = { "C:\\vs\\env_calibracion", "env_calibracion", "..\\env_calibracion" };
    std::ifstream f;
    std::string usada;
    for (const char* c : candidatos) {
        f.open(c);
        if (f.is_open()) { usada = c; break; }
    }
    if (!f.is_open()) {
        Logger::Warning("RankingMeasure: no encontre env_calibracion; el orden sigue por x");
        return;
    }
    int cargados = 0;
    int regiones = 0;
    int zonas = 0;
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);

        // ---- zona por punto (CAM2_P3=...) : mismo poligono, atado al punto ----
        if (key.find("_ROI") == std::string::npos && key.find("_VECTOR") == std::string::npos &&
            key.find("_LINEA") == std::string::npos &&
            key.rfind("CAM", 0) == 0 && key.find("_P") != std::string::npos) {
            const int zcam = std::atoi(key.c_str() + 3);
            const size_t up = key.find("_P");
            const int zpunto = std::atoi(key.c_str() + up + 2);
            if (zcam >= 1 && zcam <= 4 && zpunto > 0) {
                std::vector<std::pair<double, double>> zp;
                size_t jz = 0;
                while (jz < val.size()) {
                    const size_t scz = val.find(';', jz);
                    const std::string trozo = val.substr(jz, scz == std::string::npos ? std::string::npos : scz - jz);
                    const size_t comaz = trozo.find(',');
                    if (comaz != std::string::npos) {
                        zp.push_back({std::atof(trozo.substr(0, comaz).c_str()),
                                      std::atof(trozo.substr(comaz + 1).c_str())});
                    }
                    if (scz == std::string::npos) break;
                    jz = scz + 1;
                }
                if (zp.size() >= 3) {
                    m_zonas[zcam * 100 + zpunto] = zp;
                    ++zonas;
                }
            }
            continue;
        }

        // ---- poligono de la pista (CAM1_ROI_0_7=...) ----
        if (key.find("_ROI") != std::string::npos) {
            if (key.rfind("CAM", 0) != 0) continue;
            const int rcam = std::atoi(key.c_str() + 3);
            if (rcam < 1 || rcam > 4) continue;
            int ra = 0, rb = 1000;
            const size_t roi = key.find("_ROI");
            const std::string rresto = key.substr(roi + 4);   // "_0_7" o vacio
            if (!rresto.empty() && rresto[0] == '_') {
                const size_t us2 = rresto.find('_', 1);
                if (us2 != std::string::npos) {
                    ra = std::atoi(rresto.c_str() + 1);
                    rb = std::atoi(rresto.c_str() + us2 + 1);
                }
            }
            RegionTramo r;
            r.desde = ra;
            r.hasta = rb;
            size_t j = 0;
            while (j < val.size()) {
                const size_t sc2 = val.find(';', j);
                const std::string trozo2 = val.substr(j, sc2 == std::string::npos ? std::string::npos : sc2 - j);
                const size_t coma2 = trozo2.find(',');
                if (coma2 != std::string::npos) {
                    r.pts.push_back({std::atof(trozo2.substr(0, coma2).c_str()),
                                     std::atof(trozo2.substr(coma2 + 1).c_str())});
                }
                if (sc2 == std::string::npos) break;
                j = sc2 + 1;
            }
            if (r.pts.size() >= 3) {
                r.ok = true;
                m_regiones[rcam - 1].push_back(r);
                ++regiones;
            }
            continue;
        }

        // CAM<c>_P<p>_LINEA = raya de conteo (dos puntos normalizados en la pista)
        if (key.find("_LINEA") != std::string::npos && key.find("_P") != std::string::npos) {
            const size_t pp = key.find("_P");
            const int cnum = std::atoi(key.c_str() + 3);
            const int pnum = std::atoi(key.c_str() + pp + 2);
            std::vector<std::pair<double, double>> lp;
            size_t pos = 0;
            while (pos < val.size()) {
                const size_t finTrozo = val.find(';', pos);
                const std::string trozo = val.substr(pos, finTrozo == std::string::npos ? std::string::npos : finTrozo - pos);
                const size_t coma = trozo.find(',');
                if (coma != std::string::npos) {
                    lp.emplace_back(std::atof(trozo.substr(0, coma).c_str()),
                                    std::atof(trozo.substr(coma + 1).c_str()));
                }
                if (finTrozo == std::string::npos) break;
                pos = finTrozo + 1;
            }
            if (lp.size() >= 2) {
                LineaConteo lc;
                lc.x1 = lp[0].first; lc.y1 = lp[0].second;
                lc.x2 = lp[1].first; lc.y2 = lp[1].second;
                lc.ok = true;
                m_lineasPunto[cnum * 100 + pnum] = lc;
                Logger::Info("RankingMeasure: raya de conteo CAM" + std::to_string(cnum) +
                             "_P" + std::to_string(pnum));
            }
            continue;
        }

        // CAM<c>_P<p>_VECTOR = polilinea del tramo de ese punto (mismo criterio que la ROI)
        if (key.find("_VECTOR") != std::string::npos && key.find("_P") != std::string::npos) {
            const size_t pp = key.find("_P");
            const int cnum = std::atoi(key.c_str() + 3);
            const int pnum = std::atoi(key.c_str() + pp + 2);
            std::vector<std::pair<double, double>> vpts;
            size_t pos = 0;
            while (pos < val.size()) {
                const size_t finTrozo = val.find(';', pos);
                const std::string trozo = val.substr(pos, finTrozo == std::string::npos ? std::string::npos : finTrozo - pos);
                const size_t coma = trozo.find(',');
                if (coma != std::string::npos) {
                    vpts.emplace_back(std::atof(trozo.substr(0, coma).c_str()),
                                      std::atof(trozo.substr(coma + 1).c_str()));
                }
                if (finTrozo == std::string::npos) break;
                pos = finTrozo + 1;
            }
            if (vpts.size() >= 2) {
                VectorTramo vp;
                vp.ok = true;
                vp.desde = 0;
                vp.hasta = 1000;
                double suma = 0.0;
                for (size_t k = 0; k < vpts.size(); ++k) {
                    vp.pts.push_back(vpts[k]);
                    if (k > 0) {
                        const double dx = (vpts[k].first - vpts[k - 1].first) * 1920.0;
                        const double dy = (vpts[k].second - vpts[k - 1].second) * 1080.0;
                        suma += std::sqrt(dx * dx + dy * dy);
                    }
                    vp.arco.push_back(suma);
                }
                m_vectoresPunto[cnum * 100 + pnum] = vp;
                Logger::Info("RankingMeasure: vector por punto CAM" + std::to_string(cnum) +
                             "_P" + std::to_string(pnum) + " con " +
                             std::to_string(vp.pts.size()) + " puntos");
            }
            continue;
        }
        if (key.find("TRACK_VECTOR") == std::string::npos) continue;
        if (key.rfind("CAM", 0) != 0) continue;
        const int cam = std::atoi(key.c_str() + 3);
        if (cam < 1 || cam > 4) continue;
        int a = 0, b = 1000;
        const size_t tv = key.find("TRACK_VECTOR");
        const std::string resto = key.substr(tv + 12);   // "_44_59" o vacio
        if (!resto.empty() && resto[0] == '_') {
            const size_t us = resto.find('_', 1);
            if (us != std::string::npos) {
                a = std::atoi(resto.c_str() + 1);
                b = std::atoi(resto.c_str() + us + 1);
            }
        }
        std::vector<std::pair<double, double>> pts;
        size_t i = 0;
        while (i < val.size()) {
            const size_t sc = val.find(';', i);
            const std::string trozo = val.substr(i, sc == std::string::npos ? std::string::npos : sc - i);
            const size_t coma = trozo.find(',');
            if (coma != std::string::npos) {
                pts.push_back({std::atof(trozo.substr(0, coma).c_str()),
                               std::atof(trozo.substr(coma + 1).c_str())});
            }
            if (sc == std::string::npos) break;
            i = sc + 1;
        }
        if (pts.size() < 2) continue;
        VectorTramo t;
        t.desde = a;
        t.hasta = b;
        t.x0 = pts.front().first;
        t.y0 = pts.front().second;
        const double dx = pts.back().first - pts.front().first;
        const double dy = pts.back().second - pts.front().second;
        const double norma = std::sqrt(dx * dx + dy * dy);
        if (norma < 1e-6) continue;
        t.dx = dx / norma;
        t.dy = dy / norma;
        // Polilinea completa + largo de arco acumulado. AvanceEn() sale por
        // "return -1.0" si estos dos vectores no estan cargados, y entonces el orden
        // cae a la cuerda primer->ultimo punto: en la camara 2 esa cuerda apunta al
        // reves (polilinea 2,004 vs cuerda 0,282 = 7,12x) y su ranking sale invertido.
        t.pts = pts;
        t.arco.assign(pts.size(), 0.0);
        for (size_t k = 1; k < pts.size(); ++k) {
            const double sx = pts[k].first - pts[k - 1].first;
            const double sy = pts[k].second - pts[k - 1].second;
            t.arco[k] = t.arco[k - 1] + std::sqrt(sx * sx + sy * sy);
        }
        t.ok = true;
        m_vectores[cam - 1].push_back(t);
        ++cargados;
    }
    Logger::Info("RankingMeasure: " + std::to_string(cargados) +
                 " vectores de pista, " + std::to_string(regiones) +
                 " regiones ROI y " + std::to_string(zonas) +
                 " zonas por punto cargados de " + usada);
}

RankingMeasure::RegionTramo RankingMeasure::RegionDe(int camera, int64_t epochMs) const {
    RegionTramo vacio;
    if (camera < 0 || camera >= 4) return vacio;
    const auto& v = m_regiones[camera];
    if (v.empty()) return vacio;
    const int64_t ini = m_raceInicioMs.load();
    if (ini <= 0) return vacio;
    const double sec = static_cast<double>(epochMs - ini) / 1000.0;
    for (const auto& t : v) {
        if (sec >= t.desde && sec < t.hasta) return t;
    }
    // Fuera de todos los bloques (p.ej. si la rafaga se estira mas alla del ultimo
    // tramo calibrado): se usa el bloque MAS CERCANO en el tiempo, no se rinde.
    const auto* mejor = &v.front();
    double mejorDist = 1e9;
    for (const auto& t : v) {
        const double d = (sec < t.desde) ? (t.desde - sec) : (sec - t.hasta);
        if (d < mejorDist) { mejorDist = d; mejor = &t; }
    }
    return *mejor;
}

// Punto en poligono (ray casting), coordenadas normalizadas 0..1.
int RankingMeasure::EnZonaCuenta(int camera, int punto, const FrameSample& s) const {
    auto it = m_zonas.find((camera + 1) * 100 + punto);
    if (it == m_zonas.end()) {
        return static_cast<int>(s.dets.size());   // sin zona: cuenta todo
    }
    int n = 0;
    for (const auto& d : s.dets) {
        if (EnZona(camera, punto, d.x / 1920.0, d.y / 1080.0)) {
            ++n;
        }
    }
    return n;
}

// Punto en poligono de la zona del punto (coordenadas normalizadas 0..1).
bool RankingMeasure::EnZona(int camera, int punto, double cx, double cy) const {
    if (!m_filtrarPorRoi) return true;
    auto it = m_zonas.find((camera + 1) * 100 + punto);
    if (it == m_zonas.end()) return true;
    const auto& pts = it->second;
    const size_t n = pts.size();
    bool dentro = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = pts[i].first, yi = pts[i].second;
        const double xj = pts[j].first, yj = pts[j].second;
        if ((yi > cy) != (yj > cy)) {
            const double xc = xi + (cy - yi) * (xj - xi) / (yj - yi);
            if (cx < xc) {
                dentro = !dentro;
            }
        }
    }
    return dentro;
}

bool RankingMeasure::EnPista(int camera, int64_t epochMs, double cx, double cy) const {
    if (!m_filtrarPorRoi) return true;
    const RegionTramo r = RegionDe(camera, epochMs);
    if (!r.ok) return true;          // sin poligono para ese bloque: no se filtra
    bool dentro = false;
    const size_t n = r.pts.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = r.pts[i].first, yi = r.pts[i].second;
        const double xj = r.pts[j].first, yj = r.pts[j].second;
        if ((yi > cy) != (yj > cy) &&
            cx < (xj - xi) * (cy - yi) / (yj - yi) + xi) {
            dentro = !dentro;
        }
    }
    return dentro;
}

// Largo de arco del punto del recorrido mas cercano a (cx,cy). Se recorre la
// polilinea entera: es lo unico que da el sentido correcto cuando la pista curva.
double RankingMeasure::AvanceEn(int camera, int64_t epochMs, double cx, double cy,
                                double arcoPrevio, double maxPaso) const {
    const VectorTramo t = VectorDe(camera, epochMs);
    if (!t.ok || t.pts.size() < 2 || t.arco.size() != t.pts.size()) return -1.0;
    double mejorDist = 1e18;
    double mejorArco = -1.0;
    for (size_t i = 1; i < t.pts.size(); ++i) {
        const double a0 = t.arco[i - 1];
        const double a1 = t.arco[i];
        const double segLargo = a1 - a0;
        if (segLargo <= 1e-12) continue;
        // Ventana de alcance: con arcoPrevio >= 0 solo se considera el trozo del
        // recorrido al que la esfera puede llegar en UN cuadro. Sin esto el punto mas
        // cercano salta al tramo final cuando las dos puntas del recorrido pasan
        // cerca (medido: la punta final queda a 541 px del inicio y el salto es de
        // 3656 px en un cuadro; 16 cambios de puesto >= 5 en el pasaje).
        double uLo = 0.0, uHi = 1.0;
        if (arcoPrevio >= 0.0 && maxPaso > 0.0) {
            uLo = (arcoPrevio - maxPaso - a0) / segLargo;
            uHi = (arcoPrevio + maxPaso - a0) / segLargo;
            if (uLo < 0.0) uLo = 0.0;
            if (uHi > 1.0) uHi = 1.0;
            if (uLo > uHi) continue;      // tramo inalcanzable desde el cuadro anterior
        }
        // Pared ROI: si la recta entre la esfera y este tramo sale del poligono, el
        // tramo no es alcanzable aunque este mas cerca en pixeles.
        if (m_paredRoi) {
            bool cruza = false;
            for (int kk = 1; kk <= 8; ++kk) {
                const double tt = kk / 9.0;
                const double mx = cx + (t.pts[i - 1].first - cx) * tt;
                const double my = cy + (t.pts[i - 1].second - cy) * tt;
                const bool dentroPared = (m_puntoActual > 0)
                    ? EnZona(camera, m_puntoActual, mx, my)
                    : EnPista(camera, epochMs, mx, my);
                if (!dentroPared) { cruza = true; break; }
            }
            if (cruza) continue;
        }
        const double ax = t.pts[i - 1].first, ay = t.pts[i - 1].second;
        const double bx = t.pts[i].first, by = t.pts[i].second;
        const double vx = bx - ax, vy = by - ay;
        const double largo2 = vx * vx + vy * vy;
        double u = 0.0;
        if (largo2 > 1e-12) {
            u = ((cx - ax) * vx + (cy - ay) * vy) / largo2;
            if (u < uLo) u = uLo;
            if (u > uHi) u = uHi;
        }
        const double px = ax + u * vx, py = ay + u * vy;
        const double dx = cx - px, dy = cy - py;
        const double dist = dx * dx + dy * dy;
        if (dist < mejorDist) {
            mejorDist = dist;
            mejorArco = a0 + u * segLargo;
        }
    }
    // Ningun tramo alcanzable: la esfera se queda donde estaba (no se teletransporta).
    if (mejorArco < 0.0) return (arcoPrevio >= 0.0) ? arcoPrevio : -1.0;
    return mejorArco;
}

RankingMeasure::VectorTramo RankingMeasure::VectorDe(int camera, int64_t epochMs) const {
    // Primero el vector DEL PUNTO que se esta midiendo: la ROI y el vector cuelgan
    // del punto, no del reloj (si no, con otro preset o sin carrera el vector no
    // pasa por las esferas y el avance sale 0).
    if (m_puntoActual > 0) {
        const auto itp = m_vectoresPunto.find((camera + 1) * 100 + m_puntoActual);
        if (itp != m_vectoresPunto.end() && itp->second.ok && itp->second.pts.size() >= 2) {
            return itp->second;
        }
    }
    VectorTramo vacio;
    if (camera < 0 || camera >= 4) return vacio;
    const auto& v = m_vectores[camera];
    if (v.empty()) return vacio;
    const int64_t ini = m_raceInicioMs.load();
    if (ini <= 0) return vacio;
    const double sec = static_cast<double>(epochMs - ini) / 1000.0;
    for (const auto& t : v) {
        if (sec >= t.desde && sec < t.hasta) return t;
    }
    // Fuera de todos los bloques (p.ej. si la rafaga se estira mas alla del ultimo
    // tramo calibrado): se usa el bloque MAS CERCANO en el tiempo, no se rinde.
    const auto* mejor = &v.front();
    double mejorDist = 1e9;
    for (const auto& t : v) {
        const double d = (sec < t.desde) ? (t.desde - sec) : (sec - t.hasta);
        if (d < mejorDist) { mejorDist = d; mejor = &t; }
    }
    return *mejor;
}

bool RankingMeasure::ResolvePositions(int camera, int punto, std::vector<FrameSample>& samples,
                                      std::vector<int>& posiciones,
                                      int64_t& resolvedEpochMs) {
    // Tabla de pistas de la camara de este punto (una por camara desde 13-nov).
    auto& m_tracks = m_tracksCam[(camera >= 0 && camera < kMaxCameras) ? camera : 0];
    posiciones.clear();
    resolvedEpochMs = 0;
    m_puntoActual = punto;

    // CAMARA EN MOVIMIENTO: los cuadros del barrido quedan en la toma pero NO se
    // rastrean ni votan. Se marcan, no se borran: la cuenta total y el clip siguen
    // cuadrando con lo que la camara mando.
    {
        const int64_t movHasta = m_movHasta[camera].load(std::memory_order_relaxed);
        if (movHasta > 0) {
            for (auto& s : samples) {
                if (s.epochMs < movHasta) {
                    s.enBarrido = true;
                }
            }
        }
    }

    // ZONA DEL PUNTO: se recorta la muestra al poligono de (camara, punto). Si el
    // punto no tiene zona calibrada, no se filtra nada.
    if (m_filtrarPorRoi && punto > 0) {
        auto itz = m_zonas.find((camera + 1) * 100 + punto);
        if (itz != m_zonas.end()) {
            for (auto& s : samples) {
                std::vector<FrameDetection> dentro;
                dentro.reserve(s.dets.size());
                for (const auto& d : s.dets) {
                    // d.x/d.y vienen en pixeles; la zona esta en 0..1
                    if (EnZona(camera, punto, d.x / 1920.0, d.y / 1080.0)) {
                        dentro.push_back(d);
                    }
                }
                s.dets.swap(dentro);
            }
        }
    }

    ResetIdentity();

    // 1) Seguir cada esfera por MOVIMIENTO (x,y) y acumular el voto de clase.
    //    No se ordena por x: con las esferas rodando, la x cambia cuadro a cuadro
    //    y usarla como identidad inventa esferas nuevas (medido 3-nov: 13 ids
    //    para 10 esferas).
    int validos = 0;
    for (auto& sample : samples) {
        sample.trackOf.assign(sample.dets.size(), -1);
        if (sample.dets.empty() || sample.enBarrido) {
            continue;
        }
        MatchFrameByMotion(sample);
        resolvedEpochMs = sample.epochMs;
        ++validos;
    }
    if (resolvedEpochMs == 0 || validos == 0) {
        return false;
    }

    // 2) Esferas reales: las que aparecen en una parte razonable de la rafaga.
    //    Una deteccion de un solo cuadro es ruido; exigir presencia evita que un
    //    falso positivo entre al ranking. Ningun cuadro suelto decide nada.
    // Antes: 40 % de la rafaga. En un pasaje eso descarta esferas reales (medido en el
    // punto 7: 4 de 11 pistas cruzaban en menos del 40 % y se perdian). Ahora se exige
    // una presencia absoluta de 15 cuadros (0,5 s), o la mitad si la ventana es corta:
    // alcanza para descartar fantasmas de 1-3 cuadros sin perder esferas que cruzan.
    // El minimo no puede crecer con el largo de la toma: con 900 cuadros pedia 120
    // de presencia por esfera y una esfera que cruza en 2-3 s no calificaba
    // (medido 12-nov-2026: punto 1 con 5.752 cajas publico cero). Tope: 30 (1 s).
    // Tope de 15 cuadros (0,5 s de presencia) para la pista: con 30 se perdian
    // esferas que cruzan el tramo en menos de un segundo (medido 12-nov-2026).
    const int minVentana = (validos / 6) > 12 ? ((validos / 6) < 15 ? validos / 6 : 15) : 12;
    const int minimoFrames = (validos < 30) ? (std::max)(2, validos / 2) : minVentana;
    std::vector<size_t> idx;
    idx.reserve(m_tracks.size());
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        // Continuidad: la pista no puede tener huecos grandes. Si entre su primera y
        // ultima aparicion hay mas de 40 ms por cuadro detectado, es una deteccion
        // intermitente (fantasma), no una esfera.
        const int64_t spanMs = m_tracks[i].lastMs - m_tracks[i].firstMs;
        const bool continua = spanMs <= static_cast<int64_t>(m_tracks[i].frames) * 40;
        if (m_tracks[i].frames >= minimoFrames && continua) {
            idx.push_back(i);
        }
    }
    int n = static_cast<int>(idx.size());
    if (n == 0) {
        return false;
    }

    // 3) DEDUPE POR CLASE. Dos pistas con la MISMA clase que nunca aparecen juntas en
    //    un cuadro son la misma esfera partida por un reinicio de identidad: se queda
    //    la de mas cuadros y se suman sus votos al voto final. Si coinciden en algun
    //    cuadro son dos esferas distintas (una mal etiquetada) y se dejan las dos:
    //    cual sobra lo decide el voto, no este paso.
    if (n > 1) {
        std::vector<int> clase(n, -1);
        std::unordered_map<int, int> idAK;
        idAK.reserve(static_cast<size_t>(n) * 2);
        for (int k = 0; k < n; ++k) {
            clase[k] = ArgMaxClass(m_tracks[idx[k]]);
            idAK[m_tracks[idx[k]].id] = k;
        }
        std::vector<bool> juntas(static_cast<size_t>(n) * static_cast<size_t>(n), false);
        std::vector<int> presentes;
        for (const auto& s : samples) {
            presentes.clear();
            for (int id : s.trackOf) {
                auto it = idAK.find(id);
                if (it != idAK.end()) {
                    presentes.push_back(it->second);
                }
            }
            for (size_t p = 0; p < presentes.size(); ++p) {
                for (size_t q = p + 1; q < presentes.size(); ++q) {
                    if (presentes[p] == presentes[q]) {
                        continue;
                    }
                    juntas[static_cast<size_t>(presentes[p]) * n + presentes[q]] = true;
                    juntas[static_cast<size_t>(presentes[q]) * n + presentes[p]] = true;
                }
            }
        }
        // Desempate de clases repetidas: dos pistas que COEXISTEN y comparten la clase
        // dominante son dos esferas distintas mal clasificadas (el tracker las separo
        // bien, el voto no). La de menos votos toma su segunda mejor clase si esta
        // libre. Sin esto la lista sale con una clase repetida y el que publica pierde
        // el puesto: medido 12-nov-2026 en el punto 2, [2,9,10,4,8,8] -> 6 posiciones.
        {
            auto votoMax = [&](int k) {
                const SphereTrack& tr = m_tracks[idx[k]];
                int m = 0;
                for (int c = 0; c < kTargetSpheres; ++c) {
                    if (tr.votes[c] > m) m = tr.votes[c];
                }
                return m;
            };
            std::vector<int> orden(n);
            for (int k = 0; k < n; ++k) orden[k] = k;
            std::sort(orden.begin(), orden.end(), [&](int a, int b) {
                return votoMax(a) > votoMax(b);
            });
            std::vector<bool> tomada(static_cast<size_t>(kTargetSpheres) + 1, false);
            for (int oi = 0; oi < n; ++oi) {
                const int k = orden[oi];
                const int c = clase[k];
                if (c < 0) continue;
                if (!tomada[static_cast<size_t>(c)]) {
                    tomada[static_cast<size_t>(c)] = true;
                    continue;
                }
                const SphereTrack& tr = m_tracks[idx[k]];
                int mejor = -1;
                int mejorV = 0;
                for (int c2 = 0; c2 < kTargetSpheres; ++c2) {
                    if (tomada[static_cast<size_t>(c2)]) continue;
                    if (tr.votes[c2] > mejorV) {
                        mejorV = tr.votes[c2];
                        mejor = c2;
                    }
                }
                const int tope = votoMax(k);
                if (mejor >= 0 && tope > 0 && mejorV * 4 >= tope) {
                    Logger::Info("RankingMeasure: clase repetida " + std::to_string(c) +
                                 " -> la pista de menos votos pasa a " + std::to_string(mejor));
                    clase[k] = mejor;
                    tomada[static_cast<size_t>(mejor)] = true;
                }
            }
        }

        std::vector<int> padre(n);
        for (int k = 0; k < n; ++k) {
            padre[k] = k;
        }
        auto raiz = [&padre](int x) {
            while (padre[x] != x) {
                padre[x] = padre[padre[x]];
                x = padre[x];
            }
            return x;
        };
        for (int i = 0; i < n; ++i) {
            if (clase[i] < 0) {
                continue;
            }
            for (int j = i + 1; j < n; ++j) {
                if (clase[j] != clase[i]) {
                    continue;
                }
                if (juntas[static_cast<size_t>(i) * n + j]) {
                    continue;   // se ven juntas: son dos esferas distintas
                }
                const int ri = raiz(i);
                const int rj = raiz(j);
                if (ri != rj) {
                    padre[rj] = ri;
                }
            }
        }
        std::unordered_map<int, int> ganador;   // raiz -> pista con mas cuadros
        for (int k = 0; k < n; ++k) {
            const int r = raiz(k);
            auto it = ganador.find(r);
            if (it == ganador.end() ||
                m_tracks[idx[k]].frames > m_tracks[idx[it->second]].frames) {
                ganador[r] = k;
            }
        }
        std::vector<size_t> idx2;
        idx2.reserve(idx.size());
        for (int k = 0; k < n; ++k) {
            if (ganador[raiz(k)] == k) {
                idx2.push_back(idx[k]);
            }
        }
        if (idx2.size() < idx.size()) {
            Logger::Info("RankingMeasure: dedupe por clase: " +
                         std::to_string(idx.size() - idx2.size()) +
                         " pista(s) fusionadas (quedan " + std::to_string(idx2.size()) + ")");
        }
        idx.swap(idx2);
        n = static_cast<int>(idx.size());
        if (n == 0) {
            return false;
        }
    }

    auto posDeId = [&idx, this](int id) -> int {
        for (int k = 0; k < static_cast<int>(idx.size()); ++k) {
            if (m_tracks[idx[k]].id == id) {
                return k;
            }
        }
        return -1;
    };

    // 3) ORDEN votado por pares: en cuantos cuadros la esfera a estuvo delante de
    //    la b. Se usa la rafaga completa, no un cuadro elegido a dedo.
    std::vector<std::vector<int>> delante(n, std::vector<int>(n, 0));
    std::vector<double> sumaX(n, 0.0);
    std::vector<int> vecesX(n, 0);

    for (const auto& sample : samples) {
        if (sample.enBarrido) {
            continue;
        }
        // Avance sobre la pista (no x): el vector de cada tramo dice para donde
        // va el grupo. Sin calibracion cae a x, como antes.
        const VectorTramo tv = VectorDe(camera, sample.epochMs);
        std::vector<std::pair<int, float>> presentes;
        presentes.reserve(sample.dets.size());
        for (size_t d = 0; d < sample.dets.size(); ++d) {
            const int p = posDeId(sample.trackOf[d]);
            if (p >= 0) {
                // La esfera ARRASTRA su arco: se proyecta con el arco del cuadro
                // anterior como testigo y solo puede moverse kMaxPasoArco. Asi no
                // puede tomar el tramo final sin haber pasado por los anteriores.
                SphereTrack& esfera = m_tracks[idx[p]];
                const double arco = AvanceEn(camera, sample.epochMs, sample.dets[d].x,
                                             sample.dets[d].y, esfera.arco, kMaxPasoArco);
                if (arco >= 0.0) {
                    esfera.arco = arco;
                }
                const double avance = (arco >= 0.0) ? arco
                    : (tv.ok ? ((sample.dets[d].x / 1920.0) - tv.x0) * tv.dx +
                               ((sample.dets[d].y / 1080.0) - tv.y0) * tv.dy
                             : sample.dets[d].x);
                presentes.push_back({p, static_cast<float>(avance)});
            }
        }
        for (size_t i = 0; i < presentes.size(); ++i) {
            sumaX[presentes[i].first] += presentes[i].second;
            vecesX[presentes[i].first] += 1;
            for (size_t j = 0; j < presentes.size(); ++j) {
                if (i != j && presentes[i].second > presentes[j].second) {
                    delante[presentes[i].first][presentes[j].first] += 1;
                }
            }
        }
    }

    // Puntos tipo Copeland (cuantas esferas le gano a cada una) + x media de
    // desempate: la mas adelantada primero.
    struct Ranked {
        int pos;
        int puntos;
        double xMedia;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(n);
    for (int i = 0; i < n; ++i) {
        int puntos = 0;
        for (int j = 0; j < n; ++j) {
            if (i != j && delante[i][j] > delante[j][i]) {
                ++puntos;
            }
        }
        const double xMedia = (vecesX[i] > 0) ? (sumaX[i] / vecesX[i]) : 0.0;
        ranked.push_back({i, puntos, xMedia});
    }
    std::sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.puntos != b.puntos) {
            return a.puntos > b.puntos;
        }
        return a.xMedia > b.xMedia;
    });

    // 4) Identidad: la clase mas votada de cada esfera en TODA la rafaga.
    std::vector<int> claseUsada(kTargetSpheres, 0);
    int repetidas = 0;
    std::ostringstream detalle;
    detalle << "RankingMeasure: voto (esferas=" << n << " de " << m_tracks.size()
            << " pistas, cuadros=" << validos << "):";
    int puesto = 0;
    for (const auto& r : ranked) {
        const SphereTrack& track = m_tracks[idx[r.pos]];
        const int cls = ArgMaxClass(track);
        if (cls < 0) {
            continue;
        }
        ++puesto;
        if (claseUsada[cls]++ > 0) {
            ++repetidas;
        }
        detalle << " [" << puesto << "]B" << cls << "(" << track.votes[cls] << "/"
                << track.frames << "v," << r.puntos << "p)";
        posiciones.push_back(cls + 1);   // el payload va 1..10 (B0 = canica 1)
    }
    Logger::Info(detalle.str());

    // Dos esferas con la misma clase = no hay ranking posible: se descarta entero
    // en vez de publicar un puesto repetido.
    if (repetidas > 0) {
        if (m_config.strictClasses) {
            Logger::Warning("RankingMeasure: " + std::to_string(repetidas) +
                            " clase(s) asignadas a dos esferas distintas, no se publica");
            posiciones.clear();
            return false;
        }
        Logger::Warning("RankingMeasure: " + std::to_string(repetidas) +
                        " clase(s) repetidas; strict_classes=false, se publica PROVISORIO");
    }

    return !posiciones.empty();
}

void RankingMeasure::MatchFrameByMotion(FrameSample& sample) {
    if (m_tracks.size() > kMaxTracks) {
        // PODA en vez de reinicio total: con ~2,3 identidades nuevas por cuadro el
        // mapa se llenaba en ~10 cuadros y ResetIdentity mataba las pistas en curso.
        // Medido 12-nov-2026 en el punto 2: 1.492 cajas dentro de la zona y CERO
        // posiciones porque ninguna pista llegaba a los 30 cuadros minimos.
        const int64_t corte = sample.epochMs - 2000;
        m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
                          [corte](const SphereTrack& t) { return t.lastMs < corte; }),
                       m_tracks.end());
        if (m_tracks.size() > kMaxTracks) {
            Logger::Warning("RankingMeasure: demasiadas identidades provisorias, se reinician");
            ResetIdentity();
        }
    }

    std::vector<bool> usado(m_tracks.size(), false);

    for (size_t d = 0; d < sample.dets.size(); ++d) {
        const FrameDetection& det = sample.dets[d];

        size_t best = m_tracks.size();
        float bestDist = 0.0f;
        for (size_t i = 0; i < m_tracks.size(); ++i) {
            if (i < usado.size() && usado[i]) {
                continue;
            }
            const SphereTrack& tr = m_tracks[i];
            const float dx = det.x - tr.lastX;
            const float dy = det.y - tr.lastY;
            const float dist = std::sqrt(dx * dx + dy * dy);
            // Puerta por movimiento: entre cuadros la esfera se corre ~12 px a
            // 30 fps, asi que medio diametro sobra y no alcanza a la vecina (que
            // esta a ~0.8 diametros).
            const float diam = (std::max)(tr.lastW, tr.lastH);
            // Puerta = medio diametro, con piso de 30 px, pero tambien >= 2,2 veces
            // la velocidad de la pista. En la camara ancha (la 3) las esferas se
            // corren 40-60 px por cuadro: con la puerta fija cada cuadro creaba una
            // identidad nueva y el mapa se reiniciaba (punto 2 sin posiciones).
            const float vel = std::sqrt(tr.vx * tr.vx + tr.vy * tr.vy);
            const float gate = (std::max)((std::max)(0.55f * diam, 30.0f), 2.2f * vel);
            if (dist <= gate && (best == m_tracks.size() || dist < bestDist)) {
                best = i;
                bestDist = dist;
            }
        }

        if (best == m_tracks.size()) {
            SphereTrack nuevo;
            nuevo.id = m_nextTrackId++;
            nuevo.firstMs = sample.epochMs;
            nuevo.lastMs = sample.epochMs;
            m_tracks.push_back(nuevo);
            usado.push_back(true);
            best = m_tracks.size() - 1;
        } else {
            if (best < usado.size()) {
                usado[best] = true;
            }
        }

        SphereTrack& track = m_tracks[best];
        track.vx = det.x - track.lastX;
        track.vy = det.y - track.lastY;
        track.lastX = det.x;
        track.lastY = det.y;
        track.lastW = det.w;
        track.lastH = det.h;
        track.frames += 1;
        track.lastMs = sample.epochMs;
        if (det.ballID >= 0 && det.ballID < kTargetSpheres) {
            track.votes[det.ballID] += 1;
            track.confSum[det.ballID] += det.conf;
        }
        sample.trackOf[d] = track.id;
    }
}

int RankingMeasure::ArgMaxClass(const SphereTrack& track) {
    int best = -1;
    int bestVotes = 0;
    float bestConf = 0.0f;
    for (int c = 0; c < kTargetSpheres; ++c) {
        const int votos = track.votes[c];
        if (votos > bestVotes) {
            best = c;
            bestVotes = votos;
            bestConf = track.confSum[c];
        } else if (votos > 0 && votos == bestVotes && track.confSum[c] > bestConf) {
            best = c;               // empate: gana la clase con mas confianza
            bestConf = track.confSum[c];
        }
    }
    return best;
}

int RankingMeasure::MaxDetections(const std::vector<FrameSample>& samples) {
    int best = 0;
    for (const auto& sample : samples) {
        const int count = static_cast<int>(sample.dets.size());
        if (count > best) {
            best = count;
        }
    }
    return best;
}

int64_t RankingMeasure::NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace Measure
