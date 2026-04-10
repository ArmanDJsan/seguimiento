/**
 * StressTester.cpp
 * 
 * Implementación del módulo de pruebas de estrés
 */

#include "StressTester.h"
#include "../core/Config.h"
#include "../utils/Logger.h"
#include "../utils/GPUDiagnostics.h"
#include "../utils/ThreadOptimizer.h"
#include "../control/VideoHubClient.h"
#include "../control/VMixController.h"
#include "../control/TrackPhysicalController.h"
#include "../capture/DeckLinkSource.h"
#include "../capture/DeckLinkCapture.h"
#include "../ndi/NDIManager.h"
#include "../ai/InferenceEngine.h"
#include "../ai/ActiveCameraSelector.h"
#include "../tracking/PositionMapper.h"
#include "../tracking/BallTracker.h"
#include "../scene/SceneManager.h"
#include "../telemetry/PerformanceMonitor.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <atomic>
#include <thread>
#include <cuda_runtime.h>

// Constantes
constexpr int kMaxNDIChannels = 12;
constexpr unsigned int kDefaultWidth = 3840;
constexpr unsigned int kDefaultHeight = 2160;
constexpr int kVideoHubPrimaryOutput = 0;

StressTester::StressTester(std::chrono::seconds testDuration)
    : m_testDuration(testDuration) {
}

DiagnosticResults StressTester::RunFullDiagnostic(const Config& config) {
    DiagnosticResults results;
    auto startTime = std::chrono::high_resolution_clock::now();
    
    Logger::Info("=== INICIANDO TEST DE ESTRES / DIAGNOSTICO ===");
    
    // Crear lookup de inputs
    std::unordered_map<std::string, int> inputLookup;
    for (int i = 1; i <= 12; ++i) {
        std::stringstream ss;
        ss << "CAM_" << std::setw(2) << std::setfill('0') << i;
        inputLookup[ss.str()] = i - 1;
    }
    inputLookup["RADAR_01"] = 12;
    inputLookup["RADAR_02"] = 13;
    inputLookup["RADAR_03"] = 14;
    inputLookup["RADAR_04"] = 15;
    
    // Inicializar controladores
    VideoHubClient videoHub(config.videohubIp, config.videohubPort, inputLookup);
    std::wstring esp32IpWide(config.esp32Ip.begin(), config.esp32Ip.end());
    TrackPhysicalController trackController(esp32IpWide, config.esp32Port);
    VMixController vmix(L"127.0.0.1", 8088, 8099);
    DeckLinkSource deckLinkSource;
    deckLinkSource.Initialize(12);
    
    // =========================================================================
    // TEST 1: Conexión VideoHub
    // =========================================================================
    auto testResult = TestVideoHubConnection(videoHub);
    results.tests.push_back(testResult);
    if (!testResult.passed) results.allPassed = false;
    
    // =========================================================================
    // TEST 2: Conexión vMix
    // =========================================================================
    testResult = TestVMixConnection(vmix);
    results.tests.push_back(testResult);
    // vMix no es crítico, no marcamos allPassed = false
    
    // =========================================================================
    // TEST 3: Barrido DeckLink Streaming (puertos 1-12)
    // =========================================================================
    if (videoHub.IsConnected()) {
        testResult = TestDeckLinkSignals(videoHub, deckLinkSource, 1, 12, "Streaming (1-12)");
        results.tests.push_back(testResult);
        if (!testResult.passed) results.allPassed = false;
    }
    
    // =========================================================================
    // TEST 4: Barrido DeckLink Seguimiento (puertos 13-16)
    // =========================================================================
    if (videoHub.IsConnected()) {
        testResult = TestDeckLinkSignals(videoHub, deckLinkSource, 13, 16, "Seguimiento (13-16)");
        results.tests.push_back(testResult);
        if (!testResult.passed) results.allPassed = false;
    }
    
    // =========================================================================
    // TEST 5: Test mecánico ESP32
    // =========================================================================
    testResult = TestESP32Mechanical(trackController);
    results.tests.push_back(testResult);
    if (!testResult.passed) results.allPassed = false;
    
    // =========================================================================
    // TEST 6: Diagnósticos GPU
    // =========================================================================
    testResult = TestGPUDiagnostics();
    results.tests.push_back(testResult);
    // GPU diagnostics es informativo, no crítico
    
    // =========================================================================
    // TEST 7: Inicialización NDI
    // =========================================================================
    testResult = TestNDIInitialization();
    results.tests.push_back(testResult);
    
    std::shared_ptr<NDIManager> ndiManager;
    if (testResult.passed) {
        ndiManager = std::make_shared<NDIManager>();
        ndiManager->Initialize();
        
        // Crear senders NDI
        for (int channel = 0; channel < kMaxNDIChannels; ++channel) {
            std::ostringstream oss;
            oss << "VIB_CAM_" << std::setw(2) << std::setfill('0') << (channel + 1);
            ndiManager->CreateSender(channel, oss.str(), kDefaultWidth, kDefaultHeight, true);
        }
    } else {
        results.allPassed = false;
    }
    
    // =========================================================================
    // TEST 8: Captura real con frames
    // =========================================================================
    if (ndiManager && videoHub.IsConnected()) {
        int framesProcessed = 0;
        double avgLatency = 0.0;
        
        testResult = TestRealCapture(config, videoHub, ndiManager, framesProcessed, avgLatency);
        results.tests.push_back(testResult);
        
        results.framesProcessed = framesProcessed;
        results.avgFrameLatency_ms = avgLatency;
        
        if (!testResult.passed) results.allPassed = false;
    }
    
    // Cleanup NDI
    if (ndiManager) {
        ndiManager->ReleaseAll();
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    results.totalDuration_ms = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    
    Logger::Info("=== TEST DE ESTRES / DIAGNOSTICO COMPLETADO ===");
    
    return results;
}

TestResult StressTester::TestVideoHubConnection(VideoHubClient& videoHub) {
    auto start = std::chrono::high_resolution_clock::now();
    
    bool connected = videoHub.Connect();
    
    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();
    
    if (connected) {
        return TestResult(true, "Conexion VideoHub", 
                         "Conectado a " + videoHub.GetHost() + ":" + std::to_string(videoHub.GetPort()),
                         duration);
    } else {
        return TestResult(false, "Conexion VideoHub", 
                         "No se pudo conectar a VideoHub",
                         duration);
    }
}

TestResult StressTester::TestVMixConnection(VMixController& vmix) {
    auto start = std::chrono::high_resolution_clock::now();
    
    bool connected = vmix.ConnectTcp();
    bool inputsHealthy = vmix.CheckInputsHealthy();
    
    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();
    
    std::string details;
    if (connected && inputsHealthy) {
        details = "Conectado, todos los inputs OK";
    } else if (connected) {
        details = "Conectado, pero algunos inputs con problemas";
    } else {
        details = "No se pudo conectar a vMix (no critico)";
    }
    
    return TestResult(connected, "Conexion vMix", details, duration);
}

TestResult StressTester::TestDeckLinkSignals(VideoHubClient& videoHub,
                                              DeckLinkSource& deckLink,
                                              int startPort, int endPort,
                                              const std::string& label) {
    auto start = std::chrono::high_resolution_clock::now();
    
    std::vector<int> ports;
    for (int i = startPort; i <= endPort; ++i) {
        ports.push_back(i);
    }
    
    // Conmutar cada puerto en VideoHub
    int lockedCount = 0;
    int totalPorts = static_cast<int>(ports.size());
    std::vector<std::string> failedPorts;
    
    for (int port : ports) {
        int videoHubInput = port - 1;
        if (!videoHub.RouteInputToOutput(kVideoHubPrimaryOutput, videoHubInput)) {
            failedPorts.push_back(std::to_string(port));
            continue;
        }
    }
    
    // Esperar estabilización
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Verificar señales
    auto statuses = deckLink.GetSignalStatus(ports);
    for (const auto& status : statuses) {
        if (status.signalLocked) {
            lockedCount++;
        } else {
            std::string name = status.name.value_or("Puerto_" + std::to_string(status.index));
            failedPorts.push_back(name);
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();
    
    std::ostringstream details;
    details << lockedCount << "/" << totalPorts << " senales bloqueadas";
    if (!failedPorts.empty()) {
        details << " (fallaron: ";
        for (size_t i = 0; i < failedPorts.size(); ++i) {
            if (i > 0) details << ", ";
            details << failedPorts[i];
        }
        details << ")";
    }
    
    bool passed = (lockedCount == totalPorts);
    return TestResult(passed, "Barrido DeckLink " + label, details.str(), duration);
}

TestResult StressTester::TestESP32Mechanical(TrackPhysicalController& trackController) {
    auto start = std::chrono::high_resolution_clock::now();
    
    bool testOK = trackController.ejecutarTest();
    
    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();
    
    if (testOK) {
        return TestResult(true, "Test Mecanico ESP32", "Test /test completado OK", duration);
    } else {
        return TestResult(false, "Test Mecanico ESP32", "Fallo en test mecanico", duration);
    }
}

TestResult StressTester::TestGPUDiagnostics() {
    auto start = std::chrono::high_resolution_clock::now();
    
    // Verificar HAGS
    auto schedInfo = GPUDiagnostics::CheckHardwareAcceleratedScheduling();
    
    // Verificar PCIe
    auto pcieInfo = GPUDiagnostics::GetPCIeInfo(0);
    
    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();
    
    std::ostringstream details;
    details << "HAGS: " << (schedInfo.hagsEnabled ? "Habilitado" : "Deshabilitado");
    details << ", PCIe: x" << pcieInfo.linkWidth << " Gen" << pcieInfo.linkGen;
    details << " (" << std::fixed << std::setprecision(1) << pcieInfo.bandwidth_gbps << " GB/s)";
    
    bool optimal = schedInfo.hagsEnabled && pcieInfo.isOptimal;
    return TestResult(optimal, "Diagnosticos GPU", details.str(), duration);
}

TestResult StressTester::TestNDIInitialization() {
    auto start = std::chrono::high_resolution_clock::now();
    
    auto testNDI = std::make_shared<NDIManager>();
    bool initialized = testNDI->Initialize();
    testNDI->ReleaseAll();
    
    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();
    
    if (initialized) {
        return TestResult(true, "Inicializacion NDI", "Biblioteca NDI cargada correctamente", duration);
    } else {
        return TestResult(false, "Inicializacion NDI", "Fallo al inicializar NDI", duration);
    }
}

TestResult StressTester::TestRealCapture(const Config& config,
                                          VideoHubClient& videoHub,
                                          std::shared_ptr<NDIManager> ndiManager,
                                          int& framesProcessed,
                                          double& avgLatency) {
    auto start = std::chrono::high_resolution_clock::now();
    
    Logger::Info("Iniciando captura real por " + std::to_string(m_testDuration.count()) + " segundo(s)...");
    
    // Contadores atómicos para el callback
    std::atomic<int> frameCount{0};
    std::atomic<double> totalLatency{0.0};
    std::atomic<bool> stopCapture{false};
    
    // Detectar dispositivos DeckLink
    int numDevices = DeckLinkCapture::EnumerateDevices();
    if (numDevices == 0) {
        return TestResult(false, "Captura Real", "No se encontraron dispositivos DeckLink", 0);
    }
    
    // Inicializar canales de captura
    std::vector<std::unique_ptr<DeckLinkCapture>> captureChannels;
    int channelsToInit = (std::min)(numDevices, kMaxNDIChannels);
    
    for (int i = 0; i < channelsToInit; ++i) {
        auto capture = std::make_unique<DeckLinkCapture>();
        std::string channelName = "Channel_" + std::to_string(i + 1);
        
        if (capture->Initialize(i, channelName)) {
            // Handler de frame simplificado para el test
            capture->SetFrameReadyHandler([ndiManager, &frameCount, &totalLatency, &stopCapture]
                                         (const VideoChannel& channel, cudaStream_t stream) {
                if (stopCapture.load()) return;
                
                auto frameStart = std::chrono::high_resolution_clock::now();
                
                // Enviar frame a NDI (validar pipeline)
                ndiManager->SendUYVYFrame(
                    channel.channelID,
                    channel.cudaYUVBuffer,
                    channel.width,
                    channel.height,
                    stream
                );
                
                auto frameEnd = std::chrono::high_resolution_clock::now();
                double latency = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
                
                frameCount.fetch_add(1);
                totalLatency.store(totalLatency.load() + latency);
            });
            
            capture->Start();
            captureChannels.push_back(std::move(capture));
        }
    }
    
    if (captureChannels.empty()) {
        return TestResult(false, "Captura Real", "No se pudo inicializar ningun canal de captura", 0);
    }
    
    // Ejecutar captura por el tiempo especificado
    std::this_thread::sleep_for(m_testDuration);
    
    // Detener captura
    stopCapture.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Dar tiempo para que terminen los frames en vuelo
    
    for (auto& capture : captureChannels) {
        capture->Stop();
    }
    captureChannels.clear();
    
    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count();
    
    // Calcular resultados
    framesProcessed = frameCount.load();
    avgLatency = (framesProcessed > 0) ? (totalLatency.load() / framesProcessed) : 0.0;
    
    std::ostringstream details;
    details << framesProcessed << " frames procesados, ";
    details << "latencia promedio: " << std::fixed << std::setprecision(2) << avgLatency << " ms";
    
    bool passed = (framesProcessed > 0);
    return TestResult(passed, "Captura Real (" + std::to_string(m_testDuration.count()) + "s)", 
                     details.str(), duration);
}

void StressTester::DisplayResults(const DiagnosticResults& results) {
    std::cout << "\n";
    std::cout << "  ======================================================================\n";
    std::cout << "  ||          RESULTADOS DEL TEST DE ESTRES / DIAGNOSTICO            ||\n";
    std::cout << "  ======================================================================\n";
    std::cout << "\n";
    
    for (const auto& test : results.tests) {
        std::string status = test.passed ? "[OK]" : "[FALLO]";
        std::string marker = test.passed ? "+" : "X";
        
        std::cout << "  " << marker << " " << status << " " << test.name << "\n";
        std::cout << "         " << test.details << "\n";
        if (test.duration_ms > 0) {
            std::cout << "         Duracion: " << std::fixed << std::setprecision(1) 
                      << test.duration_ms << " ms\n";
        }
        std::cout << "\n";
    }
    
    std::cout << "  ----------------------------------------------------------------------\n";
    
    if (results.framesProcessed > 0) {
        std::cout << "  Frames procesados: " << results.framesProcessed << "\n";
        std::cout << "  Latencia promedio: " << std::fixed << std::setprecision(2) 
                  << results.avgFrameLatency_ms << " ms\n";
    }
    
    std::cout << "  Duracion total: " << std::fixed << std::setprecision(1) 
              << results.totalDuration_ms << " ms\n";
    std::cout << "\n";
    
    if (results.allPassed) {
        std::cout << "  ======================================================================\n";
        std::cout << "  ||              RESULTADO FINAL: TODOS LOS TESTS OK               ||\n";
        std::cout << "  ======================================================================\n";
    } else {
        std::cout << "  ======================================================================\n";
        std::cout << "  ||        RESULTADO FINAL: ALGUNOS TESTS FALLARON                 ||\n";
        std::cout << "  ||        Revise los detalles arriba para mas informacion         ||\n";
        std::cout << "  ======================================================================\n";
    }
    
    std::cout << "\n";
}

void StressTester::WaitForEnter() {
    std::cout << "  Presione ENTER para volver al menu...";
    std::cin.get();
}
