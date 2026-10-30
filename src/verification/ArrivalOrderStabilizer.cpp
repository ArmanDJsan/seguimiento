/**
 * ArrivalOrderStabilizer.cpp
 *
 * Implementación del estabilizador de orden de llegada.
 */

#include "ArrivalOrderStabilizer.h"

ArrivalOrderStabilizer::ArrivalOrderStabilizer(const ArrivalStabilizerConfig& cfg)
    : m_cfg(cfg)
{
    if (m_cfg.stableFrames < 1) m_cfg.stableFrames = 1;
    if (m_cfg.minSpheres < 1) m_cfg.minSpheres = 1;
}

bool ArrivalOrderStabilizer::AddOrder(const std::vector<int>& order) {
    // Guardar la anotación (historial acotado).
    m_history.push_back(order);
    if (m_history.size() > kHistoryLimit) {
        m_history.pop_front();
    }

    m_lastReject = 0;

    // Orden vacío o con menos esferas de las exigidas: no cuenta y resetea el contador.
    if (static_cast<int>(order.size()) < m_cfg.minSpheres) {
        m_consecutiveCount = 0;
        m_prevOrder.clear();
        m_lastReject = 1;
        return false;
    }

    // Invariante de identidad: si hay un ballID repetido (swap o doble detección),
    // el orden es inválido y NUNCA debe confirmarse, sin importar cuántas esferas
    // llegaron. Se resetea el contador para evitar bloquearse en un error sistemático.
    if (HasDuplicateId(order)) {
        m_consecutiveCount = 0;
        m_prevOrder.clear();
        m_lastReject = 2;
        return false;
    }

    if (!m_prevOrder.empty() && order == m_prevOrder) {
        m_consecutiveCount++;
    } else {
        m_prevOrder = order;
        m_consecutiveCount = 1;
    }

    if (!m_confirmed && m_consecutiveCount >= m_cfg.stableFrames) {
        m_confirmed = true;
        m_confirmedOrder = order;
        return true;
    }
    return false;
}

bool ArrivalOrderStabilizer::HasDuplicateId(const std::vector<int>& order) {
    // Hasta 10 esferas: un doble for es trivial y evita depender de <unordered_set>.
    for (size_t i = 0; i < order.size(); ++i) {
        for (size_t j = i + 1; j < order.size(); ++j) {
            if (order[i] == order[j]) {
                return true;
            }
        }
    }
    return false;
}

void ArrivalOrderStabilizer::Reset() {
    m_prevOrder.clear();
    m_consecutiveCount = 0;
    m_confirmed = false;
    m_confirmedOrder.clear();
    m_history.clear();
    m_lastReject = 0;
}
