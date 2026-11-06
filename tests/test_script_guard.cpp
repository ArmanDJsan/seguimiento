/**
 * test_script_guard.cpp
 *
 * Test de host (Linux, sin Windows ni CUDA) del parseo de la guarda de la
 * compuerta en los guiones reales del repo:
 *   g++ -std=c++20 -Wall -Wextra tests/test_script_guard.cpp \
 *       src/choreography/ChoreographyScript.cpp src/utils/Logger.cpp -o /tmp/t
 *
 * Verifica que el JSON "require_spheres" / "point" / "verify_timeout_ms" del
 * ESP32Command "iniciar" llegue a los parametros del evento: si el parser los
 * ignorara, el guion abriria la compuerta sin verificar la salida (y el /arm lo
 * rechazaria por no encontrar la guarda).
 */

#include "../src/choreography/ChoreographyScript.h"

#include <cstdio>
#include <string>

namespace {

int g_fallos = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FALLO: %s\n", what.c_str());
        ++g_fallos;
    } else {
        std::printf("ok: %s\n", what.c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string path = (argc > 1) ? argv[1] : "config/choreography/medicion_tlms.json";

    Choreography::ChoreographyScript parser;
    const auto script = parser.LoadFromJsonFile(path);
    if (!script) {
        std::printf("FALLO: no se pudo parsear %s: %s\n", path.c_str(),
                    parser.GetLastError().c_str());
        return 1;
    }
    std::printf("guion: %s (%zu eventos)\n", script->metadata.name.c_str(), script->events.size());

    int iniciar = 0;
    int conGuarda = 0;
    for (const auto& event : script->events) {
        if (event.type != Choreography::EventType::ESP32Command) {
            continue;
        }
        const auto* params = std::get_if<Choreography::ESP32CommandParams>(&event.params);
        if (!params || params->command != "iniciar") {
            continue;
        }
        ++iniciar;
        if (params->requireSpheres > 0) {
            ++conGuarda;
            std::printf("iniciar: require_spheres=%d point=%d timeout=%d ms\n",
                        params->requireSpheres, params->spheresPoint, params->verifyTimeoutMs);
        } else {
            std::printf("iniciar SIN guarda\n");
        }
    }

    Check(iniciar > 0, "el guion abre la compuerta (hay un ESP32Command iniciar)");
    Check(conGuarda == iniciar, "TODOS los iniciar traen guarda");
    Check(conGuarda == 1, "hay exactamente una guarda");

    // El punto que verifica tiene que tener su MeasurePoint en el guion, y ANTES
    // del iniciar: si no, la guarda espera una medicion que nunca llega.
    bool hayMedida = false;
    for (const auto& event : script->events) {
        if (event.type != Choreography::EventType::MeasurePoint) {
            continue;
        }
        const auto* params = std::get_if<Choreography::MeasurePointParams>(&event.params);
        if (params && params->punto == 1) {
            hayMedida = true;
            std::printf("MeasurePoint punto=1 cam=%d burst=%d max=%d\n",
                        params->camera, params->burstMs, params->maxMs);
        }
    }
    Check(hayMedida, "el guion mide el punto 1 (la salida) para la guarda");

    if (g_fallos == 0) {
        std::printf("\nTODOS LOS CHEQUEOS PASARON\n");
        return 0;
    }
    std::printf("\n%d CHEQUEOS FALLARON\n", g_fallos);
    return 1;
}
