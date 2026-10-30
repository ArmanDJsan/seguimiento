/**
 * RankingPayload.h
 *
 * Contrato PURO del ranking hacia TouchDesigner (sin red, sin Windows).
 * Vive separado del transporte para poder testearlo en cualquier plataforma.
 *
 * Payload exacto:
 *   {"type":"ranking","seq":201,"canicas":[{"id":0,"puesto":3},{"id":1,"puesto":6},...]}
 *
 * Reglas:
 *  - `id`   : identificador de canica en el dominio de TouchDesigner (0..9).
 *             El tracker usa ballID 1..10; el 10 se publica como 0 y el resto
 *             queda igual (1->1, 2->2, ... 9->9).
 *  - `puesto`: 1 = primero. 0 = todavia sin dato (canica nunca vista).
 *  - El array va ordenado por `id` de TouchDesigner (0,1,2,...,9).
 *  - Si una canica no se detecta en un frame, CONSERVA su ultimo puesto
 *    conocido (el array siempre tiene 10 elementos).
 */

#pragma once

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace ranking {

// ballID del tracker (1..10) -> id de TouchDesigner (10 -> 0, resto igual)
inline int MapBallIdToTd(int ballId) {
    return ballId == 10 ? 0 : ballId;
}

/**
 * Estado del ranking publicado.
 * Mantiene el ultimo puesto conocido por ballID y decide si hubo cambio.
 */
struct RankingState {
    static constexpr int kMaxBalls = 10;

    // Indice por ballID del tracker (1..10). [0] no se usa.
    std::array<int, kMaxBalls + 1> lastPuesto{};
    uint32_t seq = 0;

    void Reset() {
        lastPuesto.fill(0);
        seq = 0;
    }

    /**
     * Absorbe el ranking del tracker (ballIDs en orden, 1er lugar primero).
     * Las canicas ausentes conservan su puesto anterior.
     * @return true si el mapa de puestos cambio respecto de lo ultimo emitido.
     */
    bool Update(const std::vector<int>& ranking) {
        std::array<int, kMaxBalls + 1> next = lastPuesto;

        for (size_t i = 0; i < ranking.size(); ++i) {
            const int id = ranking[i];
            if (id >= 1 && id <= kMaxBalls) {
                next[id] = static_cast<int>(i) + 1;
            }
        }

        const bool changed = (next != lastPuesto);
        lastPuesto = next;
        return changed;
    }

    /**
     * Serializa el estado actual. NO incrementa seq (eso lo hace el llamador
     * cuando decide emitir).
     */
    std::string BuildPayload() const {
        std::ostringstream oss;
        oss << "{\"type\":\"ranking\",\"seq\":" << seq << ",\"canicas\":[";

        // Orden por id de TouchDesigner: 0 (ballID 10), luego 1..9.
        bool first = true;
        for (int tdId = 0; tdId <= kMaxBalls - 1; ++tdId) {
            const int ballId = (tdId == 0) ? 10 : tdId;
            if (!first) {
                oss << ",";
            }
            first = false;
            oss << "{\"id\":" << tdId
                << ",\"puesto\":" << lastPuesto[ballId] << "}";
        }

        oss << "]}";
        return oss.str();
    }
};

} // namespace ranking
