/**
 * StressTester.h
 * 
 * Módulo de pruebas de estrés y diagnóstico del sistema VIB
 * Ejecuta todos los tests de hardware/software y un ciclo corto
 * de captura con frames reales para validar el pipeline completo.
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <chrono>

// Forward declarations
class VMixController;
class VideoHubClient;
class DeckLinkSource;
class TrackPhysicalController;
class NDIManager;
class DeckLinkCapture;
class InferenceEngine;
class ActiveCameraSelector;
class PerformanceMonitor;
class PositionMapper;
class BallTracker;
class SceneManager;
class RankingPublisher;

struct Config;
struct InferenceEngineConfig;

/**
 * Resultado de un test individual
 */
struct TestResult {
    bool passed;
    std::string name;
    std::string details;
    double duration_ms;
    
    TestResult() : passed(false), duration_ms(0.0) {}
    TestResult(bool p, const std::string& n, const std::string& d, double dur = 0.0)
        : passed(p), name(n), details(d), duration_ms(dur) {}
};

/**
 * Resultado consolidado de todos los tests
 */
struct DiagnosticResults {
    bool allPassed;
    std::vector<TestResult> tests;
    double totalDuration_ms;
    int framesProcessed;
    double avgFrameLatency_ms;
    
    DiagnosticResults() 
        : allPassed(true), totalDuration_ms(0.0), 
          framesProcessed(0), avgFrameLatency_ms(0.0) {}
};

/**
 * StressTester - Ejecuta diagnósticos completos del sistema
 */
class StressTester {
public:
    /**
     * Constructor
     * @param testDuration Duración del test de captura real (default: 1 segundo)
     */
    explicit StressTester(std::chrono::seconds testDuration = std::chrono::seconds(1));
    ~StressTester() = default;
    
    // Non-copyable
    StressTester(const StressTester&) = delete;
    StressTester& operator=(const StressTester&) = delete;
    
    /**
     * Ejecutar diagnóstico completo del sistema
     * Incluye: Hardware sweep, GPU diagnostics, captura real
     * @param config Configuración del sistema
     * @return Resultados consolidados
     */
    DiagnosticResults RunFullDiagnostic(const Config& config);
    
    /**
     * Mostrar resultados en consola con formato
     */
    void DisplayResults(const DiagnosticResults& results);

private:
    std::chrono::seconds m_testDuration;
    
    /**
     * Test 1: Conectividad con VideoHub
     */
    TestResult TestVideoHubConnection(VideoHubClient& videoHub);
    
    /**
     * Test 2: Conectividad con vMix
     */
    TestResult TestVMixConnection(VMixController& vmix);
    
    /**
     * Test 3: Barrido de señales DeckLink (puertos 1-12)
     */
    TestResult TestDeckLinkSignals(VideoHubClient& videoHub, 
                                    DeckLinkSource& deckLink,
                                    int startPort, int endPort,
                                    const std::string& label);
    
    /**
     * Test 4: Test mecánico ESP32
     */
    TestResult TestESP32Mechanical(TrackPhysicalController& trackController);
    
    /**
     * Test 5: Diagnósticos GPU (HAGS, PCIe)
     */
    TestResult TestGPUDiagnostics();
    
    /**
     * Test 6: Inicialización NDI
     */
    TestResult TestNDIInitialization();
    
    /**
     * Test 7: Captura real con frames durante el tiempo especificado
     * Este es el test principal que valida el pipeline completo
     */
    TestResult TestRealCapture(const Config& config,
                               VideoHubClient& videoHub,
                               std::shared_ptr<NDIManager> ndiManager,
                               int& framesProcessed,
                               double& avgLatency);
    
    /**
     * Esperar a que el usuario presione ENTER
     */
    void WaitForEnter();
};
