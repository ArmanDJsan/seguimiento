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

    // 1. Publisher UDP del punto (1 datagrama por punto)
    m_publisher = std::make_unique<RankingPublisher>(pubConfig);
    if (!m_publisher->Initialize()) {
        Logger::Error("RankingMeasure: RankingPublisher no inicializo, se aborta");
        m_publisher.reset();
        return false;
    }

    // 2. Stream propio del medidor (NO inferenceStream de las PTZ)
    cudaError_t streamErr = cudaStreamCreate(&m_inferenceStream);
    if (streamErr != cudaSuccess) {
        Logger::Error("RankingMeasure: cudaStreamCreate fallo: " +
                      std::string(cudaGetErrorString(streamErr)));
        m_publisher->Shutdown();
        m_publisher.reset();
        m_inferenceStream = nullptr;
        return false;
    }

    // 3. Instancia propia de InferenceEngine: contexto de ejecucion y buffers
    //    propios, para no compartir el tensor indice 0 con el camino de las PTZ.
    m_inferenceReady = m_inferenceEngine.Initialize(inferenceConfig);
    if (!m_inferenceReady) {
        Logger::Warning("RankingMeasure: InferenceEngine propia no inicializo; "
                        "las mediciones no van a poder resolver puntos");
    }

    // 4. Buffer propio de device para el frame a inferir (se reserva una sola vez)
    cudaError_t allocErr = cudaMalloc(&m_deviceFrame, kMaxFrameBytes);
    if (allocErr != cudaSuccess) {
        Logger::Error("RankingMeasure: cudaMalloc del frame propio fallo: " +
                      std::string(cudaGetErrorString(allocErr)));
        m_deviceFrame = nullptr;
        m_deviceFrameBytes = 0;
        Shutdown();   // libera stream y publisher; sin buffer propio no se mide
        return false;
    }
    m_deviceFrameBytes = kMaxFrameBytes;

    // 5. Hilo medidor
    m_worker = std::jthread([this](std::stop_token stopToken) {
        WorkerThreadFunc(stopToken);
    });

    m_initialized = true;
    m_stopRequested.store(false);
    Logger::Info("RankingMeasure: inicializado (cola tope=" +
                 std::to_string(kQueueCapacity) + ", anillo=" +
                 std::to_string(FrameRing::kSlots) + " frames/camara, sin ROI)");
    return true;
}

void RankingMeasure::Shutdown() {
    // 1. Parar el hilo medidor antes de liberar anillos/GPU
    m_stopRequested.store(true);
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_queueCv.notify_all();
        m_worker.join();
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

    // 3. Buffer propio de device
    if (m_deviceFrame) {
        cudaFree(m_deviceFrame);
        m_deviceFrame = nullptr;
        m_deviceFrameBytes = 0;
    }

    // 4. Stream propio
    if (m_inferenceStream) {
        cudaStreamDestroy(m_inferenceStream);
        m_inferenceStream = nullptr;
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
        if (m_rings[cameraID]->FrameBytes() > m_deviceFrameBytes) {
            Logger::Warning("RankingMeasure: el frame de la camara " +
                            std::to_string(cameraID) + " (" +
                            std::to_string(m_rings[cameraID]->FrameBytes()) +
                            " bytes) no entra en el buffer propio (" +
                            std::to_string(m_deviceFrameBytes) + " bytes); no se podra medir");
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

void RankingMeasure::ClipStart(int punto, unsigned int width, unsigned int height) {
    m_clipRecording = false;
    m_clipFailed = false;
    m_clipFrames = 0;
    m_clipPunto = punto;
    m_clipW = width;
    m_clipH = height;
    m_clipDir.clear();

    if (!m_config.saveClip) {
        return;
    }

    // Sin ffmpeg no se graba: un .uyvy suelto no le sirve a nadie.
    m_clipFfmpeg = ExpandEnvVars(m_config.ffmpegCmd);
    const std::string probe = "\"" + m_clipFfmpeg + "\" -version >nul 2>&1";
    if (system(probe.c_str()) != 0) {
        Logger::Warning("RankingMeasure: save_clip activado pero no se pudo ejecutar ffmpeg (" +
                        m_clipFfmpeg + "); no se graba. Corregir ranking_measure.ffmpeg_cmd");
        m_clipFailed = true;
        return;
    }
    Logger::Info("RankingMeasure: ffmpeg = " + m_clipFfmpeg);

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
    if (!m_clipRecording && m_clipFrames == 0) {
        return;
    }
    m_clipRecording = false;
    if (m_clipFrames == 0) {
        return;
    }

    const std::string raw = m_clipDir + "\\clip.uyvy";
    const std::string mp4 = m_clipDir + ".mp4";
    const std::string logf = m_clipDir + "\\ffmpeg.log";

    // Volcado unico del crudo acumulado en RAM.
    {
        FILE* f = nullptr;
        if (fopen_s(&f, raw.c_str(), "wb") == 0 && f != nullptr) {
            if (!m_clipBuffer.empty()) {
                fwrite(m_clipBuffer.data(), 1, m_clipBuffer.size(), f);
            }
            fclose(f);
        } else {
            Logger::Warning("RankingMeasure: no se pudo volcar clip.uyvy en " + m_clipDir);
            m_clipFailed = true;
        }
    }
    m_clipBuffer.clear();
    m_clipBuffer.shrink_to_fit();

    long long crudoBytes = 0;
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExA(raw.c_str(), GetFileExInfoStandard, &fad)) {
            crudoBytes = (static_cast<long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        }
    }

    const std::string ffmpeg = m_clipFfmpeg.empty() ? ExpandEnvVars(m_config.ffmpegCmd) : m_clipFfmpeg;
    const std::string cmdLine = "\"" + ffmpeg + "\" -y -v error -f rawvideo -pix_fmt uyvy422 -s " +
                                std::to_string(m_clipW) + "x" + std::to_string(m_clipH) +
                                " -framerate " + std::to_string(m_config.saveFps) +
                                " -i \"" + raw + "\" -c:v libx264 -pix_fmt yuv420p -crf 20 \"" + mp4 + "\"";

    // El MP4 se arma en un hilo APARTE. ffmpeg tarda ~1 s y bloqueaba el hilo
    // medidor: la medicion siguiente arrancaba tarde y, como el anillo guarda 8
    // cuadros, los de su ventana ya no existian (medido 3-nov-2026: 3 de 9
    // puntos juntaron 1-9 cuadros y el clip salio vacio).
    const int cuadros = m_clipFrames;
    const int punto = m_clipPunto;
    m_clipFrames = 0;

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

void RankingMeasure::ProcessJob(const MeasureJob& job) {
    FrameRing* ring = GetRing(job.camera);
    if (!ring || !ring->IsInitialized()) {
        Logger::Warning("RankingMeasure: punto=" + std::to_string(job.punto) +
                        " sin anillo de frames para camara " + std::to_string(job.camera));
        m_droppedCount++;
        return;
    }
    if (!m_inferenceReady || !m_deviceFrame) {
        Logger::Warning("RankingMeasure: punto=" + std::to_string(job.punto) +
                        " sin inferencia/buffer propio, se descarta");
        m_droppedCount++;
        return;
    }

    // Evidencia: el clip arranca al entrar al punto y se cierra en CUALQUIER
    // salida de esta funcion (la guardia llama a ClipStop en su destructor).
    ClipStart(job.punto, ring->Width(), ring->Height());
    struct ClipGuard {
        RankingMeasure* self;
        ~ClipGuard() { if (self) self->ClipStop(); }
    } clipGuard{this};

    const int64_t t0 = job.requestedEpochMs;
    const int64_t burstEnd = t0 + job.burstMs;
    const int64_t hardEnd = t0 + job.maxMs;

    std::vector<FrameSample> samples;

    // 1. Pre-roll: ultimo frame sellado ANTES del disparo (para eso existe el anillo).
    // Se busca solo dentro de la ventana viva del anillo (kSlots frames).
    int64_t preRollIndex = -1;
    {
        const int64_t latest = ring->LatestIndex();
        const int64_t oldest = latest - FrameRing::kSlots + 1;
        for (int64_t i = latest; i >= 0 && i >= oldest; --i) {
            int64_t ts = 0;
            if (ring->TryGetTimestamp(i, ts) && ts <= t0) {
                preRollIndex = i;
                break;
            }
        }
    }
    if (preRollIndex >= 0) {
        FrameSample preRollSample;
        if (IngestFrame(ring, preRollIndex, preRollSample)) {
            samples.push_back(std::move(preRollSample));
        }
    }

    // 2. Rafaga corta [t0, t0+burst_ms]
    int64_t next = (preRollIndex >= 0) ? (preRollIndex + 1) : 0;
    next = CollectWindow(ring, next, t0, burstEnd, samples, false);

    // 3. Si no estan las 10 esferas, seguir hasta max_ms o hasta completarlas
    if (MaxDetections(samples) < kTargetSpheres && hardEnd > burstEnd) {
        CollectWindow(ring, next, burstEnd, hardEnd, samples, true);
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

bool RankingMeasure::IngestFrame(FrameRing* ring, int64_t index, FrameSample& out) {
    if (!m_deviceFrame || ring->FrameBytes() > m_deviceFrameBytes) {
        return false;   // el frame no entra en el buffer propio
    }

    int64_t epochMs = 0;
    if (!ring->CopyToDevice(index, m_deviceFrame, m_deviceFrameBytes, m_inferenceStream, epochMs)) {
        return false;
    }

    // Evidencia: el mismo cuadro que se infiere se guarda en disco (si esta activado).
    ClipPush(m_deviceFrame, ring->FrameBytes());

    std::vector<BallDetection> detections = m_inferenceEngine.ProcessFrameUYVY(
        m_deviceFrame,
        ring->CameraID(),
        ring->Width(),
        ring->Height(),
        m_inferenceStream);

    if (detections.empty()) {
        out.epochMs = epochMs;
        out.dets.clear();
        return true;
    }

    // Una esfera = una posicion. El motor puede devolver la MISMA esfera dos veces:
    // con la misma clase (duplicado) o con DOS clases distintas (confusion 2<->4).
    // Agrupar por ballID no sirve: en el log real hay 3 esferas distintas etiquetadas "2".
    {
        const float W = static_cast<float>(ring->Width());
        const float H = static_cast<float>(ring->Height());
        constexpr float kMismaEsferaRatio = 0.60f;   // centros a menos de 0.6 diametros = la misma esfera

        std::vector<BallDetection> orden = detections;
        std::sort(orden.begin(), orden.end(),
                  [](const BallDetection& a, const BallDetection& b) {
                      return a.confidence > b.confidence;
                  });

        std::vector<BallDetection> unique;
        unique.reserve(orden.size());
        for (const auto& det : orden) {
            bool duplicada = false;
            for (const auto& kept : unique) {
                const float dx = (det.x - kept.x) * W;
                const float dy = (det.y - kept.y) * H;
                const float dist = std::sqrt(dx * dx + dy * dy);
                const float diam = (std::max)(kept.width * W, kept.height * H);
                if (dist < kMismaEsferaRatio * diam) { duplicada = true; break; }
            }
            if (!duplicada) unique.push_back(det);
        }

        // Trazabilidad: dos esferas DISTINTAS con la misma clase = confusion del modelo.
        for (size_t i = 0; i < unique.size(); ++i) {
            for (size_t j = i + 1; j < unique.size(); ++j) {
                if (unique[i].ballID == unique[j].ballID) {
                    Logger::Warning("RankingMeasure: clase repetida en la rafaga B" +
                                    std::to_string(unique[i].ballID) +
                                    " (confusion del modelo, se conservan ambas)");
                }
            }
        }
        detections.swap(unique);
    }

    // Si el modelo devuelve mas de 10, quedarse con las de mayor confianza.
    if (detections.size() > static_cast<size_t>(kTargetSpheres)) {
        std::sort(detections.begin(), detections.end(),
                  [](const BallDetection& a, const BallDetection& b) {
                      return a.confidence > b.confidence;
                  });
        detections.resize(static_cast<size_t>(kTargetSpheres));
    }

    // Se guarda la deteccion completa (posicion, tamano, clase y confianza) en
    // PIXELES: el seguimiento por movimiento y el voto de clase necesitan la
    // clase, que antes se tiraba aca mismo.
    const float Wpx = static_cast<float>(ring->Width());
    const float Hpx = static_cast<float>(ring->Height());

    out.epochMs = epochMs;
    out.dets.clear();
    out.dets.reserve(detections.size());
    for (const auto& det : detections) {
        FrameDetection f;
        f.x = det.x * Wpx;
        f.y = det.y * Hpx;
        f.w = det.width * Wpx;
        f.h = det.height * Hpx;
        f.ballID = det.ballID;
        f.conf = det.confidence;
        out.dets.push_back(f);
    }
    return true;
}

int64_t RankingMeasure::CollectWindow(FrameRing* ring, int64_t fromIndex,
                                               int64_t fromTs, int64_t untilTs,
                                               std::vector<FrameSample>& samples,
                                               bool stopWhenComplete) {
    int64_t cursor = (fromIndex > 0) ? fromIndex : 0;
    // No arrancar por debajo de la ventana viva del anillo: esos frames ya no existen.
    {
        const int64_t oldest = ring->LatestIndex() - FrameRing::kSlots + 1;
        if (cursor < oldest) {
            cursor = (oldest > 0) ? oldest : 0;
        }
    }
    bool finalDrainDone = false;
    int timestampRetries = 0;

    for (;;) {
        if (m_stopRequested.load()) {
            return cursor;              // shutdown: no seguir esperando frames
        }
        // Ingerir todo lo disponible dentro de la ventana
        for (;;) {
            const int64_t latest = ring->LatestIndex();
            if (cursor > latest) {
                break;
            }

            int64_t ts = 0;
            if (!ring->TryGetTimestamp(cursor, ts)) {
                if (cursor <= latest - FrameRing::kSlots) {
                    ++cursor;          // slot reciclado: frame perdido
                    timestampRetries = 0;
                    continue;
                }
                // posible escritura en curso: reintentar el mismo indice
                if (++timestampRetries > kTimestampRetries) {
                    ++cursor;
                    timestampRetries = 0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            timestampRetries = 0;

            if (ts < fromTs) {
                ++cursor;
                continue;
            }
            if (ts > untilTs) {
                break;                 // fuera de esta ventana
            }

            FrameSample sample;
            if (IngestFrame(ring, cursor, sample)) {
                samples.push_back(std::move(sample));
            }
            ++cursor;
        }

        if (NowEpochMs() >= untilTs) {
            if (finalDrainDone) {
                return cursor;
            }
            // Una pasada mas: el frame con ts<=untilTs puede estar entrando.
            std::this_thread::sleep_for(std::chrono::milliseconds(kFinalDrainMs));
            finalDrainDone = true;
            continue;
        }

        if (stopWhenComplete && MaxDetections(samples) >= kTargetSpheres) {
            return cursor;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
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
