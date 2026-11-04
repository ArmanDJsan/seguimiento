/**
 * RankingMeasure.cpp
 *
 * Medidor por evento (fase 1). Ver RankingMeasure.h para el contrato
 * y las reglas duras.
 *
 * Flujo por evento:
 *   1. El motor de coreografia encola (Enqueue: push a una cola de tope 8).
 *   2. El hilo medidor toma el trabajo, busca el frame de pre-roll en el anillo,
 *      y va ingiriendo frames [t0, t0+burst_ms]; si no resolvio las 10 esferas
 *      sigue hasta t0+max_ms o hasta completarlas.
 *   3. Por cada frame: cudaMemcpyAsync a un buffer propio -> inferenceEngine
 *      propio (contexto de ejecucion propio) -> lista de x.
 *   4. Resolucion: emparejar esferas entre frames por vecino mas cercano en x
 *      (identidad provisoria) y ordenar por la MEDIANA de x (mas adelantada =
 *      posicion 1). No se resuelve por un frame suelto.
 *   5. Un solo sendto() por punto con el orden final.
 */

#include "RankingMeasure.h"
#include "../utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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
constexpr size_t kMaxTracks = 40;       // techo de identidades provisorias

std::atomic<RankingMeasure*> g_attachedCoordinator{nullptr};

// Definida mas abajo en este mismo namespace anonimo; se declara aca porque
// Initialize() la usa antes de su definicion.
std::string ExpandEnvVars(const std::string& in);

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

// ============================================================================
// RankingMeasure
// ============================================================================

RankingMeasure::RankingMeasure() {
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
    m_worker = std::jthread([this](std::stop_token stopToken) {
        WorkerThreadFunc(stopToken);
    });

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
                      : std::string(" (no se reserva RAM de carrera)")));
    return true;
}

void RankingMeasure::Shutdown() {
    // 1. Parar el hilo medidor antes de liberar anillos/GPU
    m_stopRequested.store(true);
    ClipStop();   // que no quede un clip abierto al apagar
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_queueCv.notify_all();
        m_worker.join();
    }

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
        m_queue.clear();
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

void RankingMeasure::Enqueue(int camera, int punto, int burstMs, int maxMs) {
    if (!m_initialized) {
        Logger::Warning("RankingMeasure: evento MeasurePoint ignorado (medidor no inicializado)");
        return;
    }

    MeasureJob job;
    job.camera = camera;
    job.punto = punto;
    job.burstMs = (burstMs > 0) ? burstMs : m_config.burstMs;
    job.maxMs = (maxMs > 0) ? maxMs : m_config.maxMs;
    if (job.maxMs < job.burstMs) {
        job.maxMs = job.burstMs;
    }
    job.requestedEpochMs = NowEpochMs();

    // La ventana se anota ANTES de encolar: el worker no puede procesar el
    // trabajo sin verla (el clip lo abre el hilo de captura al verla).
    {
        std::lock_guard<std::mutex> lock(m_clipMutex);
        m_ventanaCam = camera;
        m_ventanaPunto = job.punto;
    }

    {
        // Solo un push a la cola: nunca se espera al hilo medidor.
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_queue.size() >= kQueueCapacity) {
            const MeasureJob& dropped = m_queue.front();
            Logger::Warning("RankingMeasure: cola llena, se descarta la medicion mas vieja "
                            "(punto=" + std::to_string(dropped.punto) + ", camara=" +
                            std::to_string(dropped.camera) + ")");
            m_queue.pop_front();
            m_droppedCount++;
        }
        m_queue.push_back(job);
    }
    m_queueCv.notify_one();

    // La ventana se anota ACA (instantaneo, sin I/O): el clip lo abre el hilo de
    // captura con el primer cuadro y lo cierra el hilo medidor al vencer.

    Logger::Debug("RankingMeasure: encolado punto=" + std::to_string(job.punto) +
                  " camara=" + std::to_string(job.camera) +
                  " burst=" + std::to_string(job.burstMs) + "ms" +
                  " max=" + std::to_string(job.maxMs) + "ms");
}

void RankingMeasure::ResetIdentity() {
    m_tracks.clear();
    m_nextTrackId = 1;
    Logger::Info("RankingMeasure: identidades provisorias reiniciadas");
}

// ----------------------------------------------------------------------------
// Hilo medidor
// ----------------------------------------------------------------------------

void RankingMeasure::WorkerThreadFunc(std::stop_token stopToken) {
    Logger::Info("RankingMeasure: hilo medidor iniciado");
    while (!stopToken.stop_requested()) {
        MeasureJob job;
        if (!PopJob(job)) {
            continue;
        }
        if (stopToken.stop_requested()) {
            break;
        }
        ProcessJob(job);
    }
    Logger::Info("RankingMeasure: hilo medidor detenido");
}

bool RankingMeasure::PopJob(MeasureJob& job) {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_queueCv.wait_for(lock, std::chrono::milliseconds(20), [this] {
        return !m_queue.empty();
    });
    if (m_queue.empty()) {
        return false;
    }
    job = m_queue.front();
    m_queue.pop_front();
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

} // namespace

void RankingMeasure::ClipStartLocked(int punto, unsigned int width, unsigned int height) {
    m_clipRecording = false;
    m_clipFailed = false;
    m_clipFrames = 0;
    m_clipPunto = punto;
    m_clipW = width;
    m_clipH = height;
    m_clipDir.clear();

    // ffmpeg ya se probo en Initialize: aca no se arranca ningun proceso.
    if (!m_config.saveClip || !m_clipToolOk) {
        return;
    }

    CreateDirectoryA(m_config.saveDir.c_str(), nullptr);
    m_clipDir = m_config.saveDir + "\\punto_" + std::to_string(punto) + "_" + ClipStamp();
    if (!CreateDirectoryA(m_clipDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        Logger::Warning("RankingMeasure: no se pudo crear la carpeta del clip (" + m_clipDir + "), no se graba");
        m_clipFailed = true;
        return;
    }

    // UN SOLO archivo con los cuadros seguidos. ffmpeg lo lee con -f rawvideo sin
    // patrones: el patron "f_%04d.uyvy" no lo expande el demuxer rawvideo y ademas
    // el %04d se estropea al pasar por cmd/PowerShell.
    // Se comprueba que la carpeta sea escribible creando el archivo vacio; los
    // cuadros se acumulan en RAM y se vuelcan al final.
    {
        FILE* f = nullptr;
        if (fopen_s(&f, (m_clipDir + "\\clip.uyvy").c_str(), "wb") != 0 || f == nullptr) {
            Logger::Warning("RankingMeasure: no se pudo crear clip.uyvy en " + m_clipDir + "; no se graba");
            m_clipFailed = true;
            return;
        }
        fclose(f);
    }
    m_clipBuffer.clear();

    m_clipRecording = true;
    Logger::Info("RankingMeasure: grabando clip punto=" + std::to_string(punto) +
                 " en " + m_clipDir + " (max " + std::to_string(m_config.saveMaxFrames) + " cuadros)");
}

void RankingMeasure::ClipPush(const void* devicePtr, size_t bytes) {
    std::lock_guard<std::mutex> lock(m_clipMutex);
    if (!m_clipRecording || m_clipFailed || !devicePtr || bytes == 0) {
        return;
    }
    if (m_config.saveMaxFrames > 0 && m_clipFrames >= m_config.saveMaxFrames) {
        return;
    }
    if (m_clipHost.size() != bytes) {
        m_clipHost.resize(bytes);
    }
    if (cudaMemcpy(m_clipHost.data(), devicePtr, bytes, cudaMemcpyDeviceToHost) != cudaSuccess) {
        Logger::Warning("RankingMeasure: cudaMemcpy del clip fallo, se corta la grabacion");
        m_clipRecording = false;
        m_clipFailed = true;
        return;
    }
    // A RAM, no a disco: el volcado va una sola vez en ClipStop.
    if (m_clipBuffer.capacity() == 0 && m_config.saveMaxFrames > 0) {
        m_clipBuffer.reserve(static_cast<size_t>(m_config.saveMaxFrames) * bytes);
    }
    m_clipBuffer.insert(m_clipBuffer.end(), m_clipHost.begin(), m_clipHost.end());
    ++m_clipFrames;
}

void RankingMeasure::ClipStop() {
    // Se saca TODO el estado bajo el lock y se trabaja con copias: mientras esto
    // vuelca 600 MB, el hilo de captura sigue empujando cuadros de la ventana.
    std::string dir;
    std::vector<unsigned char> buffer;
    int cuadros = 0;
    int punto = 0;
    unsigned int clipW = 0;
    unsigned int clipH = 0;
    {
        std::lock_guard<std::mutex> lock(m_clipMutex);
        if (!m_clipRecording && m_clipFrames == 0) {
            return;
        }
        m_clipRecording = false;
        cuadros = m_clipFrames;
        m_clipFrames = 0;
        punto = m_clipPunto;
        clipW = m_clipW;
        clipH = m_clipH;
        dir = m_clipDir;
        buffer.swap(m_clipBuffer);
        m_clipCamera = -1;
        m_ventanaCam = -1;
        m_ventanaPunto = 0;
    }
    if (cuadros == 0) {
        return;
    }

    const std::string raw = dir + "\\clip.uyvy";
    const std::string mp4 = dir + ".mp4";
    const std::string logf = dir + "\\ffmpeg.log";

    // Volcado unico del crudo acumulado en RAM.
    {
        FILE* f = nullptr;
        if (fopen_s(&f, raw.c_str(), "wb") == 0 && f != nullptr) {
            if (!buffer.empty()) {
                fwrite(buffer.data(), 1, buffer.size(), f);
            }
            fclose(f);
        } else {
            Logger::Warning("RankingMeasure: no se pudo volcar clip.uyvy en " + dir);
            m_clipFailed = true;
        }
    }
    long long crudoBytes = 0;
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(raw.c_str(), GetFileExInfoStandard, &fad)) {
            crudoBytes = (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        }
    }

    const std::string ffmpeg = m_clipFfmpeg.empty() ? ExpandEnvVars(m_config.ffmpegCmd) : m_clipFfmpeg;
    const std::string cmdLine = "\"" + ffmpeg + "\" -y -v error -f rawvideo -pix_fmt uyvy422 -s " +
                                std::to_string(clipW) + "x" + std::to_string(clipH) +
                                " -framerate " + std::to_string(m_config.saveFps) +
                                " -i \"" + raw + "\" -c:v libx264 -pix_fmt yuv420p -crf 20 \"" + mp4 + "\"";

    // El MP4 se arma en un hilo APARTE. ffmpeg tarda ~1 s y bloqueaba el hilo
    // medidor: la medicion siguiente arrancaba tarde y, como el anillo guarda 8
    // cuadros, los de su ventana ya no existian (medido 3-nov-2026: 3 de 9
    // puntos juntaron 1-9 cuadros y el clip salio vacio).
    std::thread([cmdLine, raw, mp4, logf, crudoBytes, cuadros, punto]() {
        // Se llama a ffmpeg DIRECTO con CreateProcess, sin cmd.exe: el shell era
        // el que fallaba ("El sistema no puede encontrar la ruta") aunque la ruta
        // fuese correcta. La salida de ffmpeg va al archivo de log.
        std::string salida;
        int rc = -1;

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hLog = CreateFileA(logf.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hLog == INVALID_HANDLE_VALUE) {
            Logger::Warning("RankingMeasure: no se pudo crear " + logf + " para la salida de ffmpeg");
        }

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = (hLog != INVALID_HANDLE_VALUE) ? hLog : GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError  = (hLog != INVALID_HANDLE_VALUE) ? hLog : GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

        PROCESS_INFORMATION pi{};
        std::vector<char> buf(cmdLine.begin(), cmdLine.end());
        buf.push_back('\0');

        const BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (!ok) {
            salida = "CreateProcess fallo, error " + std::to_string(GetLastError());
        } else {
            WaitForSingleObject(pi.hProcess, 120000);
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
        if (fopen_s(&f, logf.c_str(), "rb") == 0 && f != nullptr) {
            char tmp[512];
            const size_t n = fread(tmp, 1, sizeof(tmp) - 1, f);
            tmp[n] = '\0';
            salida += tmp;
            fclose(f);
        }

        for (size_t i = 0; i < salida.size(); ++i) {
            if (salida[i] == '\n' || salida[i] == '\r' || salida[i] == '\t') salida[i] = ' ';
        }
        while (!salida.empty() && salida.back() == ' ') salida.pop_back();

        const bool mp4Ok = (rc == 0);
        if (mp4Ok) {
            DeleteFileA(raw.c_str());
            DeleteFileA(logf.c_str());
        } else {
            Logger::Warning("RankingMeasure: ffmpeg fallo (rc=" + std::to_string(rc) +
                            ", crudo=" + std::to_string(crudoBytes) + " bytes, cuadros=" +
                            std::to_string(cuadros) + "). Comando: " + cmdLine +
                            " . Salida: " + (salida.empty() ? std::string("(vacia)") : salida));
        }
        Logger::Info("RankingMeasure: clip punto=" + std::to_string(punto) +
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
//        -> hilo aparte por camara: vuelca _race_cam<N>.uyvy + ffmpeg -> MP4
// ============================================================================

namespace {

// Cuadros por segundo que se asumen para acotar el anillo por tiempo. La
// captura de la pista corre a 30p, igual que save_fps por defecto.
constexpr int kRaceFps = 30;

// Lanza un proceso escondido (ffmpeg) SIN cmd.exe, con la salida a un archivo de
// log. Es la misma invocacion que ya usa ClipStop, extraida para no duplicarla.
// Devuelve el codigo de salida (-1 si no arranco).
int RunProcessHidden(const std::string& cmdLine, const std::string& logf, std::string& salida) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hLog = CreateFileA(logf.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        Logger::Warning("RankingMeasure: no se pudo crear " + logf + " para la salida de ffmpeg");
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = (hLog != INVALID_HANDLE_VALUE) ? hLog : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = (hLog != INVALID_HANDLE_VALUE) ? hLog : GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmdLine.begin(), cmdLine.end());
    buf.push_back('\0');

    int rc = -1;
    const BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) {
        salida = "CreateProcess fallo, error " + std::to_string(GetLastError());
    } else {
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
    if (fopen_s(&f, logf.c_str(), "rb") == 0 && f != nullptr) {
        char tmp[512];
        const size_t n = fread(tmp, 1, sizeof(tmp) - 1, f);
        tmp[n] = '\0';
        salida += tmp;
        fclose(f);
    }

    for (size_t i = 0; i < salida.size(); ++i) {
        if (salida[i] == '\n' || salida[i] == '\r' || salida[i] == '\t') salida[i] = ' ';
    }
    while (!salida.empty() && salida.back() == ' ') salida.pop_back();
    return rc;
}

} // namespace

void RankingMeasure::RacePush(int camera, const void* devicePtr, size_t bytes,
                              int64_t epochMs, unsigned int width, unsigned int height) {
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

void RankingMeasure::RaceStart() {
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
        const std::string raw = m_config.saveDir + "\\_race_cam" + std::to_string(cam) + ".uyvy";
        const std::string mp4 = m_config.saveDir + "\\carrera_cam" + std::to_string(cam) +
                                "_" + stamp + ".mp4";
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

        // El volcado del crudo (GB a disco) y ffmpeg van en un hilo APARTE: el
        // hilo medidor y el de captura no esperan. El hilo no toca `this`.
        try {
            std::thread(&RankingMeasure::RaceDumpWorker, std::move(cuadros), cam, w, h,
                        m_config.saveFps, ffmpeg, raw, mp4, logf, gate).detach();
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
                                    std::string ffmpeg, std::string raw,
                                    std::string mp4, std::string logf,
                                    std::shared_ptr<RaceDumpGate> gate) {
    const size_t totalCuadros = cuadros.size();

    // 1. Volcado del crudo: un solo archivo con los cuadros seguidos. Se va
    //    liberando cada cuadro a medida que se escribe (14 GB en el peor caso).
    long long escritos = 0;
    {
        FILE* f = nullptr;
        if (fopen_s(&f, raw.c_str(), "wb") == 0 && f != nullptr) {
            while (!cuadros.empty()) {
                const std::vector<unsigned char>& datos = cuadros.front().datos;
                if (!datos.empty()) {
                    const size_t n = fwrite(datos.data(), 1, datos.size(), f);
                    escritos += static_cast<long long>(n);
                }
                cuadros.pop_front();
            }
            fclose(f);
        } else {
            Logger::Warning("RankingMeasure: no se pudo crear " + raw +
                            " para la carrera cam" + std::to_string(camera));
        }
    }

    bool mp4Ok = false;
    if (escritos > 0) {
        const std::string cmdLine = "\"" + ffmpeg + "\" -y -v error -f rawvideo -pix_fmt uyvy422 -s " +
                                    std::to_string(width) + "x" + std::to_string(height) +
                                    " -framerate " + std::to_string(fps) +
                                    " -i \"" + raw + "\" -c:v libx264 -pix_fmt yuv420p -crf 20 \"" +
                                    mp4 + "\"";
        // ffmpeg puede no poder abrir el crudo recien escrito: Windows Defender
        // lo escanea y lo deja bloqueado un momento (medido 4-nov: cam1 y cam2
        // dieron "Permission denied" con el archivo completo y cerrado). Se
        // reintenta con una pausa corta.
        std::string salida;
        int rc = -1;
        for (int intento = 0; intento < 4; ++intento) {
            salida.clear();
            rc = RunProcessHidden(cmdLine, logf, salida);
            if (rc == 0) {
                break;
            }
            Logger::Warning("RankingMeasure: carrera cam" + std::to_string(camera) +
                            " ffmpeg rc=" + std::to_string(rc) + " (intento " +
                            std::to_string(intento + 1) + " de 4); reintento en 2 s");
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        }
        mp4Ok = (rc == 0);
        if (mp4Ok) {
            DeleteFileA(raw.c_str());     // el crudo solo servia para armar el MP4
            DeleteFileA(logf.c_str());
        } else {
            Logger::Warning("RankingMeasure: ffmpeg de la carrera cam" +
                            std::to_string(camera) + " fallo (rc=" + std::to_string(rc) +
                            ", crudo=" + std::to_string(escritos) + " bytes, cuadros=" +
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
    const int64_t t0 = job.requestedEpochMs;
    const int64_t burstEnd = t0 + job.burstMs;

    // El clip ya arranco en Enqueue, en el instante del disparo. Se cierra en
    // CUALQUIER salida de esta funcion (la guardia llama a ClipStop).
    struct ClipGuard {
        RankingMeasure* self;
        ~ClipGuard() { if (self) self->ClipStop(); }
    } clipGuard{this};

    // 1. Esperar a que cierre la ventana. Mientras tanto el pipeline sigue
    //    detectando y OnDetections junta las muestras de esta camara.
    while (!m_stopRequested.load() && NowEpochMs() < burstEnd) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // La ventana ya vencio: si la camara no mando ni un cuadro, no hay que
    // dejar la marca puesta (abriria el clip de un punto posterior).
    {
        std::lock_guard<std::mutex> lock(m_clipMutex);
        if (m_ventanaCam == job.camera && m_ventanaPunto == job.punto) {
            m_ventanaCam = -1;
        }
    }

    // 2. Tomar lo que el pipeline vio en la ventana. Si no estan las 10 esferas y
    //    hay techo (max_ms), se extiende hasta ahi: la historia ya tiene esos
    //    cuadros, no hay que volver a capturar nada.
    std::vector<FrameSample> samples;
    SnapshotHistory(job.camera, t0, burstEnd, samples);
    if (MaxDetections(samples) < kTargetSpheres && job.maxMs > job.burstMs) {
        const int64_t hardEnd = t0 + job.maxMs;
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
        return;
    }

    std::vector<int> posiciones;
    int64_t resolvedEpochMs = 0;
    if (!ResolvePositions(samples, posiciones, resolvedEpochMs)) {
        Logger::Warning("RankingMeasure: punto=" + std::to_string(job.punto) +
                        " sin detecciones en la rafaga, se descarta");
        m_droppedCount++;
        return;
    }

    {
        std::ostringstream oss;
        oss << "RankingMeasure: punto=" << job.punto
            << " camara=" << job.camera
            << " frames=" << samples.size()
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

    // La salida exige EXACTAMENTE las 10 esferas. Con menos el array esta
    // incompleto; con MAS (identidades de sobra acumuladas entre los cuadros de
    // la rafaga) se publican puestos que no existen: medido 3-nov-2026, 13
    // posiciones -> {"id":0,"puesto":11}. El receptor dibuja puestos 1..10.
    if (static_cast<int>(posiciones.size()) != kTargetSpheres) {
        Logger::Warning("RankingMeasure: punto=" + std::to_string(job.punto) +
                        " resuelto con " + std::to_string(posiciones.size()) +
                        " posiciones y se exigen " + std::to_string(kTargetSpheres) +
                        " exactas, no se publica");
        m_droppedCount++;
        return;
    }

    if (m_publisher && m_publisher->PublishMeasurePoint(job.punto, resolvedEpochMs, posiciones)) {
        m_measureCount++;
    } else {
        m_droppedCount++;
    }
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
        RacePush(camera, deviceFrame, frameBytes, epochMs, width, height);
    }

    // Evidencia: si hay ventana abierta para esta camara, el cuadro va al clip.
    // Clip: si esta camara tiene una ventana pedida, el clip se abre con ESTE
    // cuadro (asi las dimensiones son las reales) y se empuja despues.
    {
        std::lock_guard<std::mutex> lock(m_clipMutex);
        if (m_ventanaCam == camera) {
            if (m_clipRecording) {
                Logger::Warning("RankingMeasure: habia un clip sin cerrar (medicion "
                                "descartada por cola llena); se descarta su evidencia");
            }
            ClipStartLocked(m_ventanaPunto, width, height);
            m_clipCamera = camera;
            m_ventanaCam = -1;
        }
    }
    bool grabar = false;
    {
        std::lock_guard<std::mutex> lock(m_clipMutex);
        grabar = m_clipRecording && (camera == m_clipCamera);
    }
    if (grabar && deviceFrame && frameBytes > 0) {
        ClipPush(deviceFrame, frameBytes);   // vuelve a chequear adentro
    }

    if (dets.empty()) {
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

bool RankingMeasure::ResolvePositions(std::vector<FrameSample>& samples,
                                      std::vector<int>& posiciones,
                                      int64_t& resolvedEpochMs) {
    posiciones.clear();
    resolvedEpochMs = 0;

    ResetIdentity();

    // 1) Seguir cada esfera por MOVIMIENTO (x,y) y acumular el voto de clase.
    //    No se ordena por x: con las esferas rodando, la x cambia cuadro a cuadro
    //    y usarla como identidad inventa esferas nuevas (medido 3-nov: 13 ids
    //    para 10 esferas).
    int validos = 0;
    for (auto& sample : samples) {
        sample.trackOf.assign(sample.dets.size(), -1);
        if (sample.dets.empty()) {
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
    const int minimoFrames = (std::max)(2, validos * 2 / 5);   // 40% de la rafaga
    std::vector<size_t> idx;
    idx.reserve(m_tracks.size());
    for (size_t i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i].frames >= minimoFrames) {
            idx.push_back(i);
        }
    }
    const int n = static_cast<int>(idx.size());
    if (n == 0) {
        return false;
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
        std::vector<std::pair<int, float>> presentes;
        presentes.reserve(sample.dets.size());
        for (size_t d = 0; d < sample.dets.size(); ++d) {
            const int p = posDeId(sample.trackOf[d]);
            if (p >= 0) {
                presentes.push_back({p, sample.dets[d].x});
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
        // Red de seguridad: sin la barrera IR no hay identidad real, y no
        // queremos que la lista de tracks crezca sin techo.
        Logger::Warning("RankingMeasure: demasiadas identidades provisorias, se reinician");
        ResetIdentity();
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
            const float gate = (std::max)(0.55f * diam, 30.0f);
            if (dist <= gate && (best == m_tracks.size() || dist < bestDist)) {
                best = i;
                bestDist = dist;
            }
        }

        if (best == m_tracks.size()) {
            SphereTrack nuevo;
            nuevo.id = m_nextTrackId++;
            m_tracks.push_back(nuevo);
            usado.push_back(true);
            best = m_tracks.size() - 1;
        } else {
            if (best < usado.size()) {
                usado[best] = true;
            }
        }

        SphereTrack& track = m_tracks[best];
        track.lastX = det.x;
        track.lastY = det.y;
        track.lastW = det.w;
        track.lastH = det.h;
        track.frames += 1;
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
