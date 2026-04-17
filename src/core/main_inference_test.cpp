/**
 * main_inference_test.cpp
 *
 * Prueba aislada de inferencia YOLO en la cámara 11
 * Captura video de la cámara de seguimiento (1080p 30fps por SDI)
 * y ejecuta inferencia para verificar detección de esferas
 *
 * Hardware de seguimiento:
 * - Cámaras de seguimiento: 1080p 30fps por SDI
 * - Modelo YOLO: entrenado para 1280x720, se puede ajustar
 * - Alternativas posibles: 1080p 60fps NDI o 4K 30fps NDI
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <iomanip>
#include <sstream>

#include <Windows.h>
#include <objbase.h>

#include "../DeckLinkAPI_h.h"
#include "../capture/DeckLinkCapture.h"
#include "../ai/InferenceEngine.h"
#include "../utils/Logger.h"
#include "../json.hpp"
#include <fstream>

using json = nlohmann::json;

// Configuración de la prueba
constexpr int kTestCameraIndex = 10;        // Cámara 11 (0-indexed = 10)
constexpr int kTestCameraID = 11;           // ID lógico de la cámara
constexpr int kTestDurationSeconds = 30;    // Duración del test en segundos
constexpr int kLogIntervalMs = 1000;        // Intervalo de log en ms

// Contadores globales para estadísticas
static std::atomic<int> g_framesProcessed{0};
static std::atomic<int> g_totalDetections{0};
static std::atomic<float> g_maxConfidence{0.0f};
static std::atomic<float> g_totalInferenceMs{0.0f};
static std::atomic<int> g_inferenceCount{0};

/**
 * Cargar configuración del motor de inferencia desde config.json
 */
InferenceEngineConfig LoadInferenceConfig() {
    InferenceEngineConfig config;
    
    std::ifstream file("config.json");
    if (!file.is_open()) {
        Logger::Warning("No se pudo abrir config.json, usando valores por defecto");
        return config;
    }
    
    try {
        json j;
        file >> j;
        
        if (j.contains("inference_engine") && j["inference_engine"].is_object()) {
            auto& ie = j["inference_engine"];
            if (ie.contains("model_path") && ie["model_path"].is_string()) {
                config.modelPath = ie["model_path"].get<std::string>();
            }
            if (ie.contains("batch_size") && ie["batch_size"].is_number()) {
                config.batchSize = ie["batch_size"].get<int>();
            }
            if (ie.contains("input_size") && ie["input_size"].is_number()) {
                config.inputWidth = ie["input_size"].get<int>();
                config.inputHeight = ie["input_size"].get<int>();
            }
            if (ie.contains("confidence_threshold") && ie["confidence_threshold"].is_number()) {
                config.confidenceThreshold = ie["confidence_threshold"].get<float>();
            }
            if (ie.contains("nms_threshold") && ie["nms_threshold"].is_number()) {
                config.nmsThreshold = ie["nms_threshold"].get<float>();
            }
            if (ie.contains("num_classes") && ie["num_classes"].is_number()) {
                config.numClasses = ie["num_classes"].get<int>();
            }
        }
        
        Logger::Info("Configuración de inferencia cargada: model=" + config.modelPath + 
                    ", input=" + std::to_string(config.inputWidth) + "x" + 
                    std::to_string(config.inputHeight));
    } catch (const std::exception& e) {
        Logger::Warning("Error al parsear config.json: " + std::string(e.what()));
    }
    
    return config;
}

/**
 * Callback de frame para procesar cada frame de la cámara 11
 */
void OnFrameReceived(
    const VideoChannel& channel,
    cudaStream_t stream,
    InferenceEngine* inferenceEngine,
    cudaStream_t inferenceStream) {
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    // Ejecutar inferencia UYVY (path optimizado sin conversión intermedia BGRA)
    std::vector<BallDetection> detections = inferenceEngine->ProcessFrameUYVY(
        channel.cudaYUVBuffer,
        channel.channelID,
        channel.width,
        channel.height,
        inferenceStream,
        channel.preprocessEvent
    );
    
    auto endTime = std::chrono::high_resolution_clock::now();
    float inferenceMs = std::chrono::duration<float, std::milli>(endTime - startTime).count();
    
    // Actualizar estadísticas
    g_framesProcessed++;
    g_totalDetections += static_cast<int>(detections.size());
    g_totalInferenceMs = g_totalInferenceMs.load() + inferenceMs;
    g_inferenceCount++;
    
    // Log de detecciones individuales (solo si hay detecciones)
    if (!detections.empty()) {
        for (const auto& det : detections) {
            // Actualizar máxima confianza
            float currentMax = g_maxConfidence.load();
            while (det.confidence > currentMax) {
                if (g_maxConfidence.compare_exchange_weak(currentMax, det.confidence)) {
                    break;
                }
            }
            
            // Log cada detección
            std::ostringstream oss;
            oss << "[DETECT] Ball=" << det.ballID 
                << " Conf=" << std::fixed << std::setprecision(2) << det.confidence
                << " Pos=(" << std::setprecision(3) << det.x << "," << det.y << ")"
                << " Size=(" << det.width << "x" << det.height << ")"
                << " InfTime=" << std::setprecision(1) << inferenceMs << "ms";
            Logger::Info(oss.str());
        }
    }
}

/**
 * Mostrar estadísticas finales del test
 */
void ShowStatistics() {
    int frames = g_framesProcessed.load();
    int detections = g_totalDetections.load();
    float maxConf = g_maxConfidence.load();
    int infCount = g_inferenceCount.load();
    float totalInfMs = g_totalInferenceMs.load();
    
    float avgInferenceMs = (infCount > 0) ? (totalInfMs / infCount) : 0.0f;
    float detectionsPerFrame = (frames > 0) ? (static_cast<float>(detections) / frames) : 0.0f;
    float fps = (frames > 0) ? (frames / static_cast<float>(kTestDurationSeconds)) : 0.0f;
    
    Logger::Info("===============================================");
    Logger::Info("         RESULTADOS DEL TEST DE INFERENCIA     ");
    Logger::Info("===============================================");
    Logger::Info("Cámara probada: CAM_" + std::to_string(kTestCameraID));
    Logger::Info("Duración del test: " + std::to_string(kTestDurationSeconds) + " segundos");
    Logger::Info("-----------------------------------------------");
    Logger::Info("Frames procesados: " + std::to_string(frames));
    Logger::Info("FPS promedio: " + std::to_string(fps));
    Logger::Info("Total de detecciones: " + std::to_string(detections));
    Logger::Info("Detecciones por frame: " + std::to_string(detectionsPerFrame));
    Logger::Info("Confianza máxima: " + std::to_string(maxConf));
    Logger::Info("Tiempo promedio de inferencia: " + std::to_string(avgInferenceMs) + " ms");
    Logger::Info("===============================================");
    
    if (detections > 0) {
        Logger::Info("✓ INFERENCIA FUNCIONANDO - Se detectaron esferas");
    } else {
        Logger::Warning("⚠ NO SE DETECTARON ESFERAS - Verificar:");
        Logger::Warning("  - Que hay esferas visibles en la cámara 11");
        Logger::Warning("  - Que el modelo está correctamente cargado");
        Logger::Warning("  - Los umbrales de confianza en config.json");
    }
}

int main(int argc, char* argv[]) {
    Logger::Init("VIB_InferenceTest");
    Logger::Info("===========================================");
    Logger::Info(" TEST AISLADO DE INFERENCIA - CAMARA 11   ");
    Logger::Info("===========================================");
    Logger::Info("");
    Logger::Info("Configuración de hardware:");
    Logger::Info("  - Cámaras de seguimiento: 1080p 30fps SDI");
    Logger::Info("  - Modelo YOLO: optimizado para 1280x720");
    Logger::Info("  - Alternativas: 1080p 60fps NDI / 4K 30fps NDI");
    Logger::Info("");
    
    // Inicializar COM para DeckLink SDK
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Logger::Error("Fallo al inicializar COM: 0x" + std::to_string(hr));
        return 1;
    }
    Logger::Info("COM inicializado para DeckLink SDK");
    
    // Enumerar dispositivos DeckLink
    int numDevices = DeckLinkCapture::EnumerateDevices();
    Logger::Info("Dispositivos DeckLink encontrados: " + std::to_string(numDevices));
    
    if (numDevices <= kTestCameraIndex) {
        Logger::Error("No hay suficientes dispositivos DeckLink. Se necesita el índice " + 
                     std::to_string(kTestCameraIndex) + " (cámara 11)");
        CoUninitialize();
        return 1;
    }
    
    // Crear stream CUDA para inferencia
    cudaStream_t inferenceStream = nullptr;
    cudaError_t cudaErr = cudaStreamCreate(&inferenceStream);
    if (cudaErr != cudaSuccess) {
        Logger::Error("Fallo al crear CUDA stream: " + std::string(cudaGetErrorString(cudaErr)));
        CoUninitialize();
        return 1;
    }
    Logger::Info("CUDA stream creado para inferencia");
    
    // Inicializar motor de inferencia
    Logger::Info("Inicializando motor de inferencia...");
    auto inferenceEngine = std::make_unique<InferenceEngine>();
    InferenceEngineConfig infConfig = LoadInferenceConfig();
    
    if (!inferenceEngine->Initialize(infConfig)) {
        Logger::Error("Fallo al inicializar InferenceEngine");
        cudaStreamDestroy(inferenceStream);
        CoUninitialize();
        return 1;
    }
    
    if (inferenceEngine->IsStubMode()) {
        Logger::Warning("⚠ InferenceEngine ejecutándose en modo STUB");
        Logger::Warning("  Las detecciones serán simuladas, no reales");
        Logger::Warning("  Verifique que el modelo TensorRT existe en: " + infConfig.modelPath);
    } else {
        Logger::Info("✓ InferenceEngine inicializado con TensorRT");
        Logger::Info("  Modelo: " + infConfig.modelPath);
        Logger::Info("  Input size: " + std::to_string(infConfig.inputWidth) + "x" + 
                    std::to_string(infConfig.inputHeight));
    }
    
    // Inicializar captura de la cámara 11
    Logger::Info("Inicializando captura de cámara 11 (índice " + 
                std::to_string(kTestCameraIndex) + ")...");
    
    auto capture = std::make_unique<DeckLinkCapture>();
    if (!capture->Initialize(kTestCameraIndex, "Camera_11_Test")) {
        Logger::Error("Fallo al inicializar DeckLinkCapture para cámara 11");
        cudaStreamDestroy(inferenceStream);
        CoUninitialize();
        return 1;
    }
    Logger::Info("✓ Captura de cámara 11 inicializada");
    
    // Configurar callback de frame
    InferenceEngine* infEnginePtr = inferenceEngine.get();
    capture->SetFrameReadyHandler([infEnginePtr, inferenceStream]
                                  (const VideoChannel& channel, cudaStream_t stream) {
        OnFrameReceived(channel, stream, infEnginePtr, inferenceStream);
    });
    
    // Iniciar captura
    Logger::Info("");
    Logger::Info("Iniciando captura y procesamiento de inferencia...");
    Logger::Info("Duración del test: " + std::to_string(kTestDurationSeconds) + " segundos");
    Logger::Info("Presione ESC para terminar antes");
    Logger::Info("");
    
    if (!capture->Start()) {
        Logger::Error("Fallo al iniciar captura");
        cudaStreamDestroy(inferenceStream);
        CoUninitialize();
        return 1;
    }
    
    // Loop principal del test
    auto testStart = std::chrono::steady_clock::now();
    auto lastLog = testStart;
    bool running = true;
    
    while (running) {
        auto now = std::chrono::steady_clock::now();
        
        // Verificar tiempo de test
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - testStart);
        if (elapsed.count() >= kTestDurationSeconds) {
            Logger::Info("Test completado (duración máxima alcanzada)");
            running = false;
            break;
        }
        
        // Verificar ESC para salir antes
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            Logger::Info("ESC presionado - terminando test");
            running = false;
            break;
        }
        
        // Log periódico de estado
        auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLog);
        if (sinceLast.count() >= kLogIntervalMs) {
            int frames = g_framesProcessed.load();
            int detections = g_totalDetections.load();
            int remaining = kTestDurationSeconds - static_cast<int>(elapsed.count());
            
            Logger::Info("[STATUS] Frames=" + std::to_string(frames) + 
                        " Detecciones=" + std::to_string(detections) +
                        " Restante=" + std::to_string(remaining) + "s");
            lastLog = now;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    // Detener captura
    Logger::Info("Deteniendo captura...");
    capture->Stop();
    
    // Mostrar estadísticas
    ShowStatistics();
    
    // Cleanup
    Logger::Info("Limpiando recursos...");
    capture.reset();
    inferenceEngine.reset();
    
    if (inferenceStream) {
        cudaStreamDestroy(inferenceStream);
    }
    
    CoUninitialize();
    
    Logger::Info("Test finalizado");
    
    // Esperar que el usuario vea los resultados
    std::cout << "\nPresione ENTER para salir...";
    std::cin.get();
    
    return 0;
}
