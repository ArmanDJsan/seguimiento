/**
 * PipelineGate.h
 *
 * Ventana en la que el VIB infiere. Fuera de carrera NO tiene sentido gastar GPU
 * (8 ms por cuadro por canal, 30 fps, aunque no haya nada que medir) ni mantener
 * caliente el motor.
 *
 * Modelo simple por tiempo, sin acoplarse a la maquina de estados:
 *   /arm   -> abre 60 s            (el armado tarda 10-20 s y hace sondas)
 *   /start -> abre race_seconds+90 (la carrera, el volcado y el cierre)
 *   /abort -> cierra
 * La captura sigue viva: el chequeo de pista del /arm necesita cuadros.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace Pipeline {

inline std::atomic<int64_t> g_inferenciaHastaMs{0};
inline std::atomic<bool> g_soloEnCarrera{true};

inline int64_t AhoraMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/** Abre (o extiende) la ventana de inferencia por N segundos. 0 = cerrar. */
inline void PedirInferencia(int segundos) {
    if (!g_soloEnCarrera.load()) {
        return;
    }
    const int64_t hasta = (segundos <= 0) ? 0 : (AhoraMs() + static_cast<int64_t>(segundos) * 1000);
    // Nunca acortar una ventana ya abierta por otro pedido mas largo.
    int64_t actual = g_inferenciaHastaMs.load();
    while (hasta > actual && !g_inferenciaHastaMs.compare_exchange_weak(actual, hasta)) {
    }
    if (segundos <= 0) {
        g_inferenciaHastaMs.store(0);
    }
}

inline bool InferenciaActiva() {
    if (!g_soloEnCarrera.load()) {
        return true;
    }
    return AhoraMs() < g_inferenciaHastaMs.load();
}

}  // namespace Pipeline
