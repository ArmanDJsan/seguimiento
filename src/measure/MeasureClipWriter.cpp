/**
 * MeasureClipWriter.cpp
 *
 * Implementacion del hilo escritor de evidencia (ver MeasureClipWriter.h).
 *
 * Paso 1 (SIEMPRE, si save_burst): el cuadro que el medidor acaba de inferir se
 * baja de GPU y se escribe como JPEG con WIC (Windows nativo, sin dependencias
 * nuevas). La conversion UYVY->BGRA es la MISMA de la opcion 9 (RunLivePreview,
 * main.cpp): ITU-R BT.601 en CPU, sin kernel propio.
 *
 * Paso 2 (SOLO si hay ffmpeg.exe): al cerrar el punto se arma el MP4 desde los
 * JPEG y, si salio bien, se borran los JPEG. Si no hay ffmpeg quedan los JPEG y
 * una linea de log. Nunca falla la medicion por esto.
 *
 * Hilo medidor: BeginClip/StageFrame/EndClip solo tocan memoria y encolan; el
 * unico acceso a GPU es un cudaMemcpyAsync D2H + cudaEventRecord sobre el stream
 * del medidor (no se sincroniza ahi).
 */

#include "MeasureClipWriter.h"
#include "../utils/Logger.h"

#include <windows.h>
#include <objbase.h>
#include <ocidl.h>
#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// WIC vive en windowscodecs.dll (parte de Windows). Se enlaza por pragma para
// no tocar las AdditionalDependencies del .vcxproj.
#pragma comment(lib, "windowscodecs.lib")

namespace Measure {

namespace {

/** AAAAMMDD-HHMMSS en hora local (mismo criterio que el resto de la app). */
std::string ClipStamp() {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    auto dos = [](unsigned v) { return (v < 10) ? ("0" + std::to_string(v)) : std::to_string(v); };
    return std::to_string(st.wYear) + dos(st.wMonth) + dos(st.wDay) + "-" +
           dos(st.wHour) + dos(st.wMinute) + dos(st.wSecond);
}

/** 1 -> "0001" */
std::string Pad4(int n) {
    std::string s = std::to_string(n);
    while (s.size() < 4) s = "0" + s;
    return s;
}

/**
 * UYVY -> BGRA en CPU (ITU-R BT.601). Es la conversion del preview en vivo
 * (main.cpp, RunLivePreview, "CPU UYVY->BGRA conversion (ITU-R BT.601)").
 * width debe ser par: UYVY empaqueta dos pixeles por macro-pixel.
 */
void UyvyToBgra(const unsigned char* src, int width, int height, unsigned char* dst) {
    const int pairs = width * height / 2;
    for (int i = 0; i < pairs; ++i) {
        const int idx = i * 4;
        const int u  = src[idx + 0];
        const int y0 = src[idx + 1];
        const int v  = src[idx + 2];
        const int y1 = src[idx + 3];

        const int c0 = y0 - 16, c1 = y1 - 16, d = u - 128, e = v - 128;
        int r0 = (298 * c0 + 409 * e + 128) >> 8;
        int g0 = (298 * c0 - 100 * d - 208 * e + 128) >> 8;
        int b0 = (298 * c0 + 516 * d + 128) >> 8;
        int r1 = (298 * c1 + 409 * e + 128) >> 8;
        int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
        int b1 = (298 * c1 + 516 * d + 128) >> 8;

        r0 = (r0 < 0) ? 0 : (r0 > 255 ? 255 : r0);
        g0 = (g0 < 0) ? 0 : (g0 > 255 ? 255 : g0);
        b0 = (b0 < 0) ? 0 : (b0 > 255 ? 255 : b0);
        r1 = (r1 < 0) ? 0 : (r1 > 255 ? 255 : r1);
        g1 = (g1 < 0) ? 0 : (g1 > 255 ? 255 : g1);
        b1 = (b1 < 0) ? 0 : (b1 > 255 ? 255 : b1);

        const int o0 = (i * 2) * 4;
        dst[o0 + 0] = static_cast<unsigned char>(b0);
        dst[o0 + 1] = static_cast<unsigned char>(g0);
        dst[o0 + 2] = static_cast<unsigned char>(r0);
        dst[o0 + 3] = 255;

        const int o1 = (i * 2 + 1) * 4;
        dst[o1 + 0] = static_cast<unsigned char>(b1);
        dst[o1 + 1] = static_cast<unsigned char>(g1);
        dst[o1 + 2] = static_cast<unsigned char>(r1);
        dst[o1 + 3] = 255;
    }
}

/** BGRA -> BGR24 (24bppBGR es el formato que el encoder JPEG de WIC acepta). */
void BgraToBgr24(const unsigned char* src, int width, int height, unsigned char* dst) {
    const size_t pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < pixels; ++i) {
        dst[i * 3 + 0] = src[i * 4 + 0];
        dst[i * 3 + 1] = src[i * 4 + 1];
        dst[i * 3 + 2] = src[i * 4 + 2];
    }
}

/** Ruta ANSI (como las arma std::filesystem::path en Windows) -> wide para WIC. */
std::wstring ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

/** Escribe el buffer BGR24 como JPEG con WIC. */
bool WriteJpegWic(IWICImagingFactory* factory, const std::string& path,
                  const unsigned char* bgr24, int width, int height, int quality) {
    if (!factory || !bgr24 || width <= 0 || height <= 0) {
        return false;
    }

    const std::wstring wpath = ToWide(path);
    if (wpath.empty()) {
        return false;
    }

    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    HRESULT hr = factory->CreateStream(&stream);

    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE);
    }
    if (SUCCEEDED(hr)) {
        hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->CreateNewFrame(&frame, &props);
    }
    if (SUCCEEDED(hr)) {
        PROPBAG2 option = {};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value;
        VariantInit(&value);
        value.vt = VT_R4;
        value.fltVal = static_cast<float>(quality) / 100.0f;
        hr = props->Write(1, &option, &value);
        VariantClear(&value);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->Initialize(props);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) {
        hr = frame->SetPixelFormat(&format);
        // Si el encoder no acepta 24bppBGR tal cual, no se escribe un JPEG con
        // un formato que no preparamos: se reporta y el escritor se apaga.
        if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) {
            hr = E_FAIL;
        }
    }
    if (SUCCEEDED(hr)) {
        const UINT stride = static_cast<UINT>(width) * 3u;
        const UINT size = stride * static_cast<UINT>(height);
        hr = frame->WritePixels(static_cast<UINT>(height), stride, size,
                                const_cast<BYTE*>(bgr24));
    }
    if (SUCCEEDED(hr)) {
        hr = frame->Commit();
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
    }

    if (frame) frame->Release();
    if (props) props->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();

    if (FAILED(hr)) {
        DeleteFileW(wpath.c_str());   // no dejar un f_NNNN.jpg a medio escribir
        return false;
    }
    return true;
}

bool FileExistsA(const std::string& path) {
    const DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/**
 * Busca ffmpeg.exe: primero junto al binario (lo que pide la tarjeta), despues
 * en PATH. Vacio = no hay ffmpeg y se dejan los JPEG.
 */
std::string FindFfmpeg() {
    char exePath[MAX_PATH] = {0};
    const DWORD n = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::string exe(exePath, n);
        const size_t pos = exe.find_last_of("\\/");
        if (pos != std::string::npos) {
            const std::string sibling = exe.substr(0, pos + 1) + "ffmpeg.exe";
            if (FileExistsA(sibling)) {
                return sibling;
            }
        }
    }

    char found[MAX_PATH] = {0};
    if (SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, found, nullptr) > 0) {
        return std::string(found);
    }
    return std::string();
}

/** Lanza un proceso oculto y espera. Devuelve false si no se pudo lanzar. */
bool RunProcessAndWait(const std::string& exe, const std::string& args, DWORD& exitCode) {
    std::string commandLine = "\"" + exe + "\" " + args;

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::vector<char> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back('\0');

    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace

// ============================================================================
// Ciclo de vida
// ============================================================================

MeasureClipWriter::MeasureClipWriter(const MeasureClipConfig& config) : m_config(config) {
    // save_dir sin separador final (las rutas se arman como <dir>/punto_...)
    while (!m_config.dir.empty() &&
           (m_config.dir.back() == '\\' || m_config.dir.back() == '/')) {
        m_config.dir.pop_back();
    }
    if (m_config.dir.empty()) {
        m_config.dir = "measure_clips";
    }
    if (m_config.maxFrames < 1) {
        m_config.maxFrames = 1;
    }
    if (m_config.jpegQuality < 1 || m_config.jpegQuality > 100) {
        m_config.jpegQuality = 85;
    }
    if (m_config.fps <= 0) {
        m_config.fps = 30;
    }
}

MeasureClipWriter::~MeasureClipWriter() {
    Stop();
}

bool MeasureClipWriter::Start() {
    if (!m_config.save) {
        return true;   // apagado: no hay hilo ni se toca el disco
    }
    if (m_writer.joinable()) {
        return true;
    }

    m_running.store(true);
    m_writer = std::jthread([this](std::stop_token stopToken) {
        WriterThreadFunc(stopToken);
    });

    Logger::Info("RankingMeasure: save_burst activado (dir=" + m_config.dir +
                 ", max=" + std::to_string(m_config.maxFrames) + " cuadros, jpeg=" +
                 std::to_string(m_config.jpegQuality) + ", fps=" +
                 std::to_string(m_config.fps) + ")");
    return true;
}

void MeasureClipWriter::Stop() {
    if (m_writer.joinable()) {
        m_writer.request_stop();
        m_cv.notify_all();
        m_writer.join();
    }
    m_running.store(false);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_frames.clear();
        m_clips.clear();
    }

    for (int i = 0; i < kSlots; ++i) {
        if (m_slots[i].done) {
            // Si quedo una copia D2H en vuelo, se espera antes de liberar el buffer.
            if (m_slots[i].inUse.load()) {
                cudaEventSynchronize(m_slots[i].done);
            }
            cudaEventDestroy(m_slots[i].done);
            m_slots[i].done = nullptr;
        }
        if (m_slots[i].host) {
            cudaFreeHost(m_slots[i].host);
            m_slots[i].host = nullptr;
        }
        m_slots[i].bytes = 0;
        m_slots[i].inUse.store(false);
    }
}

// ============================================================================
// Hilo medidor (sin I/O, sin esperas)
// ============================================================================

void MeasureClipWriter::BeginClip(int punto, unsigned int width, unsigned int height) {
    m_clipActive = false;

    if (!m_config.save || m_failed.load() || !m_running.load()) {
        return;
    }
    if (width == 0 || height == 0 || (width % 2) != 0) {
        FailOnce("dimensiones invalidas para UYVY (" + std::to_string(width) + "x" +
                 std::to_string(height) + ")");
        return;
    }

    m_clipPunto = punto;
    m_clipW = width;
    m_clipH = height;
    m_staged = 0;
    // Solo se arma el nombre: la carpeta la crea el hilo escritor.
    m_clipDir = m_config.dir + "/punto_" + std::to_string(punto) + "_" + ClipStamp();
    m_clipActive = true;
}

void MeasureClipWriter::StageFrame(const void* deviceSrc, size_t bytes, cudaStream_t stream) {
    if (!m_clipActive || m_failed.load() || !m_running.load() || !deviceSrc || bytes == 0) {
        return;
    }
    if (m_staged >= static_cast<size_t>(m_config.maxFrames)) {
        return;   // tope save_max_frames: se corta, no se encola mas
    }

    Slot* slot = AcquireSlot(bytes);
    if (!slot) {
        m_dropped++;   // los kSlots estan en vuelo: se descarta, no se espera
        return;
    }
    const int slotIndex = static_cast<int>(slot - &m_slots[0]);

    // Copia asincrona en el stream del medidor: queda ordenada DESPUES de la
    // inferencia de este cuadro (que ya sincronizo el stream) y ANTES del
    // proximo CopyToDevice, asi que m_deviceFrame no se pisa. No se sincroniza.
    if (cudaMemcpyAsync(slot->host, deviceSrc, bytes, cudaMemcpyDeviceToHost, stream) != cudaSuccess) {
        ReleaseSlot(slotIndex);
        m_dropped++;
        FailOnce("cudaMemcpyAsync del cuadro fallo");
        return;
    }
    if (cudaEventRecord(slot->done, stream) != cudaSuccess) {
        ReleaseSlot(slotIndex);
        m_dropped++;
        FailOnce("cudaEventRecord del cuadro fallo");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_frames.size() >= kQueueCapacity) {
            // Cola corta: se descarta el mas viejo (el hilo medidor no espera).
            ReleaseSlot(m_frames.front().slot);
            m_frames.pop_front();
            m_dropped++;
        }
        FrameTask task;
        task.slot = slotIndex;
        task.width = static_cast<int>(m_clipW);
        task.height = static_cast<int>(m_clipH);
        task.dir = m_clipDir;
        m_frames.push_back(std::move(task));
    }
    m_staged++;
    m_cv.notify_one();
}

void MeasureClipWriter::EndClip() {
    if (!m_clipActive) {
        return;
    }
    m_clipActive = false;
    if (m_failed.load() || !m_running.load()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ClipTask task;
        task.punto = m_clipPunto;
        task.dir = m_clipDir;
        m_clips.push_back(std::move(task));
    }
    m_cv.notify_one();
}

// ============================================================================
// Hilo escritor
// ============================================================================

void MeasureClipWriter::WriterThreadFunc(std::stop_token stopToken) {
    Logger::Info("RankingMeasure: hilo escritor de clips iniciado");

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOk = SUCCEEDED(hrCom);

    IWICImagingFactory* factory = nullptr;
    if (comOk) {
        const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            factory = nullptr;
        }
    }
    if (!factory) {
        FailOnce("WIC no disponible (CoCreateInstance de CLSID_WICImagingFactory fallo)");
    } else {
        m_wicFactory = factory;
    }

    while (!stopToken.stop_requested() && !m_failed.load()) {
        FrameTask frame;
        ClipTask clip;
        bool hasFrame = false;
        bool hasClip = false;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(20), [this] {
                return m_failed.load() || !m_frames.empty() || !m_clips.empty() ||
                       !m_running.load();
            });
            if (m_failed.load() || !m_running.load()) {
                break;
            }
            // Los cuadros van primero: un cierre de punto solo se procesa cuando
            // ya se escribieron todos los cuadros que estaban encolados.
            if (!m_frames.empty()) {
                frame = std::move(m_frames.front());
                m_frames.pop_front();
                hasFrame = true;
            } else if (!m_clips.empty()) {
                clip = std::move(m_clips.front());
                m_clips.pop_front();
                hasClip = true;
            }
        }

        if (hasFrame) {
            ProcessFrame(frame);
        } else if (hasClip) {
            ProcessClip(clip);
        }
    }

    if (factory) {
        factory->Release();
        m_wicFactory = nullptr;
    }
    if (comOk) {
        CoUninitialize();
    }
    m_running.store(false);
    Logger::Info("RankingMeasure: hilo escritor de clips detenido (cuadros descartados=" +
                 std::to_string(m_dropped.load()) + ")");
}

void MeasureClipWriter::ProcessFrame(const FrameTask& task) {
    if (task.slot < 0 || task.slot >= kSlots) {
        return;
    }
    Slot& slot = m_slots[task.slot];

    // Espera el D2H SOLO en este hilo (el medidor ya siguio con otra cosa).
    if (cudaEventSynchronize(slot.done) != cudaSuccess) {
        ReleaseSlot(task.slot);
        m_dropped++;
        FailOnce("cudaEventSynchronize del cuadro fallo");
        return;
    }
    if (!m_wicFactory) {
        ReleaseSlot(task.slot);
        return;
    }

    const int w = task.width;
    const int h = task.height;
    const size_t uyvyBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 2u;
    if (slot.bytes < uyvyBytes) {
        ReleaseSlot(task.slot);
        m_dropped++;
        FailOnce("el cuadro bajado (" + std::to_string(slot.bytes) +
                 " bytes) no alcanza para " + std::to_string(w) + "x" + std::to_string(h));
        return;
    }

    // La carpeta se crea aca: el hilo medidor nunca toca el disco.
    if (task.dir != m_currentDir) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(task.dir), ec);
        if (ec) {
            ReleaseSlot(task.slot);
            FailOnce("no se pudo crear la carpeta del clip (" + task.dir + "): " + ec.message());
            return;
        }
        m_currentDir = task.dir;
        m_currentFrame = 0;
    }

    const size_t bgraBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    const size_t bgrBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 3u;
    if (m_bgraBuf.size() < bgraBytes) m_bgraBuf.resize(bgraBytes);
    if (m_bgrBuf.size() < bgrBytes) m_bgrBuf.resize(bgrBytes);

    UyvyToBgra(slot.host, w, h, m_bgraBuf.data());
    BgraToBgr24(m_bgraBuf.data(), w, h, m_bgrBuf.data());

    const std::string path = task.dir + "/f_" + Pad4(m_currentFrame + 1) + ".jpg";
    if (!WriteJpegWic(static_cast<IWICImagingFactory*>(m_wicFactory), path,
                      m_bgrBuf.data(), w, h, m_config.jpegQuality)) {
        ReleaseSlot(task.slot);
        FailOnce("no se pudo escribir " + path);
        return;
    }

    ++m_currentFrame;
    ReleaseSlot(task.slot);
}

void MeasureClipWriter::ProcessClip(const ClipTask& task) {
    const std::filesystem::path dir(task.dir);
    int frames = 0;

    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec)) {
        ec.clear();
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec)) {
                ++frames;
            }
            if (ec) break;
        }
    }

    const std::string mp4Path = task.dir + ".mp4";
    bool mp4Ok = false;

    if (frames > 0) {
        const std::string ffmpeg = FindFfmpeg();
        if (ffmpeg.empty()) {
            Logger::Info("RankingMeasure: clip punto=" + std::to_string(task.punto) +
                         " cuadros=" + std::to_string(frames) +
                         " carpeta=" + task.dir +
                         " mp4=no (sin ffmpeg.exe junto al binario ni en PATH; quedan los JPEG)");
            return;
        }

        // Comando de la tarjeta: los JPEG del punto -> MP4 H.264.
        // El %04d va literal (no se formatea aca): lo expande ffmpeg.
        const std::string args =
            "-y -framerate " + std::to_string(m_config.fps) +
            " -i \"" + task.dir + "/f_%04d.jpg\"" +
            " -c:v libx264 -pix_fmt yuv420p -crf 23 \"" + mp4Path + "\"";

        DWORD exitCode = 1;
        if (RunProcessAndWait(ffmpeg, args, exitCode) && exitCode == 0) {
            std::error_code sizeEc;
            const auto size = std::filesystem::file_size(std::filesystem::path(mp4Path), sizeEc);
            mp4Ok = !sizeEc && size > 0;
        }

        if (mp4Ok) {
            // El MP4 quedo bien: los JPEG ya no hacen falta.
            std::error_code delEc;
            delEc.clear();
            for (const auto& entry : std::filesystem::directory_iterator(dir, delEc)) {
                if (delEc) break;
                if (entry.is_regular_file(delEc)) {
                    std::filesystem::remove(entry.path(), delEc);
                }
            }
            std::filesystem::remove(dir, delEc);   // solo si quedo vacia
        }
    }

    Logger::Info("RankingMeasure: clip punto=" + std::to_string(task.punto) +
                 " cuadros=" + std::to_string(frames) +
                 " carpeta=" + task.dir +
                 " mp4=" + (mp4Ok ? "si" : "no"));
}

void MeasureClipWriter::FailOnce(const std::string& reason) {
    bool expected = false;
    if (!m_failed.compare_exchange_strong(expected, true)) {
        return;   // ya se logueo y ya esta apagado: una sola linea
    }
    Logger::Error("RankingMeasure: escritor de clips apagado (" + reason +
                  "); la medicion sigue igual");
    m_cv.notify_all();
}

MeasureClipWriter::Slot* MeasureClipWriter::AcquireSlot(size_t bytes) {
    for (int i = 0; i < kSlots; ++i) {
        Slot& slot = m_slots[i];
        bool expected = false;
        if (!slot.inUse.compare_exchange_strong(expected, true)) {
            continue;
        }

        if (slot.host == nullptr || slot.bytes != bytes) {
            if (slot.host) {
                cudaFreeHost(slot.host);
                slot.host = nullptr;
                slot.bytes = 0;
            }
            void* host = nullptr;
            if (cudaHostAlloc(&host, bytes, cudaHostAllocDefault) != cudaSuccess) {
                slot.inUse.store(false);
                FailOnce("cudaHostAlloc de " + std::to_string(bytes) + " bytes fallo");
                return nullptr;
            }
            slot.host = static_cast<unsigned char*>(host);
            slot.bytes = bytes;
        }
        if (slot.done == nullptr) {
            if (cudaEventCreateWithFlags(&slot.done, cudaEventDisableTiming) != cudaSuccess) {
                slot.inUse.store(false);
                FailOnce("cudaEventCreate del cuadro fallo");
                return nullptr;
            }
        }
        return &slot;
    }
    return nullptr;   // todos los slots estan en vuelo
}

void MeasureClipWriter::ReleaseSlot(int slotIndex) {
    if (slotIndex >= 0 && slotIndex < kSlots) {
        m_slots[slotIndex].inUse.store(false);
    }
}

} // namespace Measure
