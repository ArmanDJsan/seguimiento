/**
 * MeasureClipWriter.h
 *
 * Evidencia visual de la rafaga de un MeasurePoint (bloque "ranking_measure":
 * save_burst). Guarda SOLO los cuadros que el medidor ingiere para ese punto:
 * desde el primer cuadro de la medicion hasta el ultimo. No es video continuo.
 *
 * Reglas duras (las mismas del medidor):
 *  - Todo el I/O (JPEG, ffmpeg) vive en un HILO ESCRITOR propio. El hilo
 *    medidor solo encola y vuelve: nunca espera disco ni compresion.
 *  - Cola corta (tope 2) con descarte del mas viejo.
 *  - Si el escritor falla (disco lleno, sin WIC, sin ffmpeg, lo que sea) se
 *    apaga solo, loguea UNA vez y la medicion sigue igual.
 *  - Default APAGADO (save_burst = false).
 *
 * Dos pasos:
 *  1. (obligatorio) los cuadros del burst como JPEG via WIC (Windows nativo,
 *     sin dependencias nuevas) en measure_clips/punto_<N>_<AAAAMMDD-HHMMSS>/,
 *     con nombre f_0001.jpg, f_0002.jpg...
 *  2. (opcional) al cerrar el punto, si hay ffmpeg.exe junto al binario o en
 *     PATH se arma el MP4 y se borran los JPEG si salio bien. Sin ffmpeg
 *     quedan los JPEG (que ya son la evidencia) y una linea de log.
 */

#pragma once

#include <cuda_runtime.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Measure {

/**
 * Configuracion del clip (campos del bloque "ranking_measure" de config.json).
 */
struct MeasureClipConfig {
    bool save = false;                 // save_burst (default APAGADO)
    std::string dir = "measure_clips"; // save_dir
    int maxFrames = 90;                // save_max_frames (3 s a 30 fps)
    int jpegQuality = 85;              // save_jpeg_quality
    int fps = 30;                      // save_fps (solo para el MP4)
};

/**
 * Escritor de evidencia de la rafaga. Un hilo, un clip a la vez.
 *
 * Hilo medidor:  BeginClip() -> StageFrame()xN -> EndClip()
 * Hilo escritor: baja el cuadro de GPU, UYVY->BGRA, JPEG (WIC), y al cerrar
 *                el punto lanza ffmpeg y limpia.
 *
 * Ninguno de los tres metodos del hilo medidor hace I/O de disco: el folder se
 * crea perezosamente en el hilo escritor cuando llega el primer cuadro.
 */
class MeasureClipWriter {
public:
    static constexpr size_t kQueueCapacity = 2;  // tope de la cola de cuadros
    static constexpr int kSlots = 3;             // 2 en cola + 1 escribiendose

    explicit MeasureClipWriter(const MeasureClipConfig& config);
    ~MeasureClipWriter();

    MeasureClipWriter(const MeasureClipWriter&) = delete;
    MeasureClipWriter& operator=(const MeasureClipWriter&) = delete;

    /** Arranca el hilo escritor. @return false si no se pudo. */
    bool Start();

    /** Para el hilo escritor y libera slots/eventos. Idempotente. */
    void Stop();

    bool IsEnabled() const { return m_config.save; }
    uint64_t GetDroppedFrames() const { return m_dropped.load(); }

    // --- llamados desde el hilo medidor (sin I/O de disco, sin esperas) ---

    /** Abre un clip para el punto. Solo calcula nombres: no toca el disco. */
    void BeginClip(int punto, unsigned int width, unsigned int height);

    /**
     * Encola el cuadro que esta en GPU (UYVY). El memcpy D2H se encola ASINCRONO
     * en `stream` (el del medidor) y el hilo medidor NO espera a que termine.
     */
    void StageFrame(const void* deviceSrc, size_t bytes, cudaStream_t stream);

    /** Cierra el clip: el hilo escritor arma el MP4 (si hay ffmpeg) y loguea. */
    void EndClip();

private:
    struct Slot {
        unsigned char* host = nullptr;
        size_t bytes = 0;
        cudaEvent_t done = nullptr;
        std::atomic<bool> inUse{false};
    };

    struct FrameTask {
        int slot = -1;
        int width = 0;
        int height = 0;
        std::string dir;   // carpeta del clip al que pertenece el cuadro
    };

    struct ClipTask {
        int punto = 0;
        std::string dir;
    };

    void WriterThreadFunc(std::stop_token stopToken);
    void ProcessFrame(const FrameTask& task);
    void ProcessClip(const ClipTask& task);
    void FailOnce(const std::string& reason);

    Slot* AcquireSlot(size_t bytes);
    void ReleaseSlot(int slotIndex);

    MeasureClipConfig m_config;

    // Slots pinned (uno por cuadro en vuelo)
    Slot m_slots[kSlots];

    // Cola corta de cuadros (tope kQueueCapacity, descarte del mas viejo)
    std::deque<FrameTask> m_frames;
    // Cierres de clip pendientes (no se descartan: son una linea por punto)
    std::deque<ClipTask> m_clips;
    std::mutex m_mutex;
    std::condition_variable m_cv;

    std::jthread m_writer;

    // Estado del clip en curso (hilo medidor)
    bool m_clipActive = false;
    int m_clipPunto = 0;
    unsigned int m_clipW = 0;
    unsigned int m_clipH = 0;
    std::string m_clipDir;
    size_t m_staged = 0;   // cuadros encolados para este clip (tope save_max_frames)

    // Estado del hilo escritor
    std::string m_currentDir;   // carpeta que se esta escribiendo
    int m_currentFrame = 0;     // ultimo f_NNNN.jpg escrito de esa carpeta
    void* m_wicFactory = nullptr;              // IWICImagingFactory* (WIC solo en el .cpp)
    std::vector<unsigned char> m_bgraBuf;      // conversion UYVY->BGRA (hilo escritor)
    std::vector<unsigned char> m_bgrBuf;       // empaquetado 24bppBGR para WIC

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_failed{false};   // el escritor se apago solo
    std::atomic<uint64_t> m_dropped{0};
};

} // namespace Measure
