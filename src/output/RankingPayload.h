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

// ballID del tracker (1..10) -> id de TouchDesigner.
// En TouchDesigner el id 0 significa "puesto indeterminado" (no deja de marcar
// cuando llegan menos canicas), asi que NINGUNA canica real puede ir con id 0.
// El id de TouchDesigner es el numero de canica: 1..10, y el ballID 1 (bola_0 del
// modelo) es la canica 10.
inline int MapBallIdToTd(int ballId) {
    return ballId == 1 ? 10 : ballId - 1;
}

/**
 * Estado del ranking publicado.
 * Mantiene el ultimo puesto conocido por ballID y decide si hubo cambio.
 */
struct RankingState {
    static constexpr int kMaxBalls = 10;

    // Indice por ballID del tracker (1..10). [0] no se usa.
    std::array<int, kMaxBalls + 1> lastPuesto{};
    // Ultimo ranking tal cual lo resolvio el medidor (ballIDs en orden, 1o primero).
    // Es lo que se serializa: las canicas resueltas van en su puesto y las que no
    // se consiguen van AL FINAL con id 0 (indeterminado), sin huecos en el medio.
    std::vector<int> lastRanking;
    uint32_t seq = 0;

    void Reset() {
        lastPuesto.fill(0);
        seq = 0;
    }

    /**
     * Absorbe el ranking del tracker (ballIDs en orden, 1er lugar primero).
     * Se reconstruye SIEMPRE desde cero: la canica que no aparece en esta
     * medicion queda en puesto 0 ("indeterminado" en Touch). Antes se arrastraba
     * el puesto anterior y eso generaba puestos repetidos en el mismo paquete
     * (medido 8-nov-2026: puesto 6 dos veces), que el receptor no puede dibujar.
     * @return true si el mapa de puestos cambio respecto de lo ultimo emitido.
     */
    bool Update(const std::vector<int>& ranking) {
        std::array<int, kMaxBalls + 1> next{};

        // 1) los puestos que trae esta medicion
        std::array<bool, kMaxBalls + 1> ocupado{};
        for (size_t i = 0; i < ranking.size() && i < static_cast<size_t>(kMaxBalls); ++i) {
            const int id = ranking[i];
            if (id >= 1 && id <= kMaxBalls && !ocupado[static_cast<int>(i) + 1]) {
                next[id] = static_cast<int>(i) + 1;
                ocupado[static_cast<int>(i) + 1] = true;
            }
        }

        // 2) las canicas que no aparecen en este punto conservan su puesto anterior
        //    SI sigue libre; asi la animacion va sumando canicas en vez de vaciarse
        //    en cada punto. Si el puesto ya esta tomado, van a 0 ("indeterminado").
        for (int id = 1; id <= kMaxBalls; ++id) {
            if (next[id] != 0) continue;
            const int prev = lastPuesto[id];
            if (prev >= 1 && prev <= kMaxBalls && !ocupado[prev]) {
                next[id] = prev;
                ocupado[prev] = true;
            }
        }

        const bool changed = (next != lastPuesto);
        lastPuesto = next;
        lastRanking = ranking;
        if (lastRanking.size() > static_cast<size_t>(kMaxBalls)) {
            lastRanking.resize(static_cast<size_t>(kMaxBalls));
        }
        return changed;
    }

    /**
     * Serializa el estado actual. NO incrementa seq (eso lo hace el llamador
     * cuando decide emitir).
     */
    std::string BuildPayload() const {
        std::ostringstream oss;
        oss << "{\"type\":\"ranking\",\"seq\":" << seq << ",\"canicas\":[";

        // Se serializa POR PUESTO: 1..10. Las canicas que se resolvieron van en su
        // orden; las que no se consiguieron van al final con id 0 (indeterminado),
        // que es como lo dibuja TouchDesigner.
        for (int puesto = 1; puesto <= kMaxBalls; ++puesto) {
            int tdId = 0;
            if (puesto <= static_cast<int>(lastRanking.size())) {
                const int ballId = lastRanking[static_cast<size_t>(puesto) - 1];
                if (ballId >= 1 && ballId <= kMaxBalls) {
                    tdId = MapBallIdToTd(ballId);
                }
            }
            if (puesto > 1) {
                oss << ",";
            }
            oss << "{\"id\":" << tdId << ",\"puesto\":" << puesto << "}";
        }

        oss << "]}";
        return oss.str();
    }
};

} // namespace ranking
