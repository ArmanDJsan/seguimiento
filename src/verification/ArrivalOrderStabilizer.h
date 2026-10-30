/**
 * ArrivalOrderStabilizer.h
 *
 * Acumula las anotaciones del orden de llegada y detecta el punto de estabilidad:
 * el mismo orden (secuencia de ballIDs) debe repetirse `stableFrames` frames
 * consecutivos, con al menos `minSpheres` esferas, para darlo por confirmado.
 */

#pragma once

#include <vector>
#include <deque>

/**
 * Configuración del estabilizador de orden de llegada
 */
struct ArrivalStabilizerConfig {
    int stableFrames = 10;   // Frames consecutivos con el mismo orden para confirmar
    int minSpheres   = 8;    // Mínimo de esferas para considerar el orden válido
};

/**
 * ArrivalOrderStabilizer - confirma el orden de llegada cuando se estabiliza
 */
class ArrivalOrderStabilizer {
public:
    explicit ArrivalOrderStabilizer(const ArrivalStabilizerConfig& cfg = ArrivalStabilizerConfig());

    /**
     * Alimenta un orden (secuencia de ballIDs en orden de llegada).
     * @return true SOLO en la transición a "confirmado" (primera vez que se estabiliza).
     */
    bool AddOrder(const std::vector<int>& order);

    bool IsConfirmed() const { return m_confirmed; }
    const std::vector<int>& GetConfirmedOrder() const { return m_confirmedOrder; }

    /** Frames consecutivos actuales con el mismo orden (para mostrar progreso). */
    int GetProgress() const { return m_consecutiveCount; }
    int GetStableFrames() const { return m_cfg.stableFrames; }

    /**
     * Motivo del último descarte (para consola):
     *   0 = ninguno, 1 = menos de minSpheres esferas, 2 = orden con ballID duplicado.
     */
    int GetLastReject() const { return m_lastReject; }

    /**
     * Invariante de identidad: el orden de llegada válido tiene un ballID por color
     * (bola_0..bola_9), es decir IDs ÚNICOS. Un swap (confusión de color) o una doble
     * detección produce un ID repetido. Es independiente de cuántas esferas lleguen
     * (8, 9 o 10), porque cada color es único.
     */
    static bool HasDuplicateId(const std::vector<int>& order);

    /** Re-armar la confirmación (limpia estado e historial). */
    void Reset();

    /** Historial de órdenes recientes (anotaciones), acotado. */
    const std::deque<std::vector<int>>& GetHistory() const { return m_history; }

private:
    ArrivalStabilizerConfig m_cfg;
    std::vector<int> m_prevOrder;
    int m_consecutiveCount = 0;
    bool m_confirmed = false;
    std::vector<int> m_confirmedOrder;
    std::deque<std::vector<int>> m_history;
    int m_lastReject = 0;   // 0 none, 1 pocas esferas, 2 ID duplicado

    static constexpr size_t kHistoryLimit = 20;
};
