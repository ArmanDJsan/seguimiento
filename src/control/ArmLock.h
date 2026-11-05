/**
 * ArmLock.h
 *
 * Regla de verificacion de un "lock" del armado automatico (/arm).
 *
 * Un lock (META o ARRIBA) se verifica con DOS camaras y criterio AND: las dos
 * tienen que medir exactamente kTargetSpheres. El orden es el del guion:
 *   1) mover CADA PTZ a su preset (el movimiento incluye el asentamiento),
 *   2) recien entonces medir TODAS las camaras,
 *   3) exigir el mismo numero en todas.
 * Se miden las dos aunque la primera ya haya fallado: el 409 tiene que decir si
 * el problema es una camara o las dos, no la primera que se encontro.
 *
 * Vive afuera de ControlApi a proposito: sin httplib, sin Windows, sin PTZ y
 * sin medidor. Quien llama aporta los dos verbos (mover y medir) como lambdas,
 * asi el orden y el veredicto se pueden probar de verdad en un test de host
 * (tests/test_arm_lock.cpp) y no solo leyendo el codigo.
 *
 * No tiene estado global ni lanza hilos: Run() corre en el hilo del handler
 * HTTP, igual que antes.
 */

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace ArmLock {

/** Una camara del lock, 1-indexed como el guion y config.json. */
struct CameraSpec {
    int camera = 0;
    int preset = 0;   // 0 = no mover la camara (se mide la vista actual)
};

/** Lo que devolvio la sonda de una camara del lock. */
struct CameraReading {
    int camera = 0;
    int preset = 0;
    int spheres = 0;
    bool measured = false;   // false = la sonda no devolvio resultado
    std::string motivo;      // por que no se pudo medir (vacio si midio)
};

/** Veredicto del lock, con el detalle por camara para el JSON y el log. */
struct Verdict {
    bool ok = false;
    int failingCamera = 0;    // camara que hizo fallar el lock (0 = no aplica)
    int failingSpheres = 0;   // esferas de ESA camara (0 si ni se midio)
    std::string detalle;      // "cam 4 = 10, cam 3 = 8"
    std::string motivo;       // veredicto en una frase, listo para el JSON
    std::vector<CameraReading> readings;
};

/**
 * Corre un lock completo con criterio AND.
 *
 * @param cameras  las camaras del lock (produccion: dos)
 * @param target   esferas exigidas en CADA camara (kTargetSpheres)
 * @param moveFn   (camera, preset, motivo) -> false si no se pudo encuadrar
 *                 (la implementacion real ya espera el asentamiento PTZ)
 * @param probeFn  (camera, preset, spheres, motivo) -> false si no hubo medicion
 * @return veredicto; ok solo si TODAS midieron y TODAS dieron 'target'
 */
template <typename MoveFn, typename ProbeFn>
Verdict Run(const std::vector<CameraSpec>& cameras, int target,
            MoveFn&& moveFn, ProbeFn&& probeFn) {
    Verdict verdict;

    if (cameras.empty()) {
        verdict.motivo = "el lock no tiene camaras configuradas";
        return verdict;
    }

    // 1) Encuadre de TODAS primero. Si se midiera entre movimientos, la
    //    segunda camara seguiria viajando cuando se mide la primera rafaga.
    for (const auto& cam : cameras) {
        std::string motivo;
        if (!moveFn(cam.camera, cam.preset, motivo)) {
            verdict.failingCamera = cam.camera;
            verdict.motivo = motivo.empty()
                ? ("no se pudo encuadrar la camara " + std::to_string(cam.camera))
                : motivo;
            return verdict;
        }
    }

    // 2) Medicion de TODAS. Un fallo de sonda no cancela la otra camara: el
    //    409 tiene que poder decir si fallo una o las dos.
    verdict.readings.reserve(cameras.size());
    for (const auto& cam : cameras) {
        CameraReading reading;
        reading.camera = cam.camera;
        reading.preset = cam.preset;
        std::string motivo;
        reading.measured = probeFn(cam.camera, cam.preset, reading.spheres, motivo);
        reading.motivo = motivo;
        verdict.readings.push_back(std::move(reading));
    }

    // 3) Detalle por camara: se arma una sola vez y va al log y al JSON (por eso
    //    no se corta en la primera que falla).
    for (size_t i = 0; i < verdict.readings.size(); ++i) {
        const CameraReading& reading = verdict.readings[i];
        if (i > 0) {
            verdict.detalle += ", ";
        }
        verdict.detalle += "cam " + std::to_string(reading.camera) + " = ";
        verdict.detalle += reading.measured ? std::to_string(reading.spheres) : "sin medicion";
    }

    // 4) AND: primero el error duro (no hubo medicion), despues el conteo.
    const CameraReading* bad = nullptr;
    for (const auto& reading : verdict.readings) {
        if (!reading.measured) {
            bad = &reading;
            break;
        }
    }
    if (!bad) {
        for (const auto& reading : verdict.readings) {
            if (reading.spheres != target) {
                bad = &reading;
                break;
            }
        }
    }

    if (!bad) {
        verdict.ok = true;
        verdict.motivo = "las " + std::to_string(verdict.readings.size()) +
                         " camaras del lock midieron " + std::to_string(target) +
                         " (" + verdict.detalle + ")";
        return verdict;
    }

    verdict.failingCamera = bad->camera;
    verdict.failingSpheres = bad->spheres;
    if (!bad->measured) {
        verdict.motivo = "no se pudo medir la camara " + std::to_string(bad->camera) +
                         ": " + (bad->motivo.empty() ? std::string("sin resultado") : bad->motivo) +
                         " (" + verdict.detalle + ")";
    } else {
        verdict.motivo = "se exigen " + std::to_string(target) +
                         " esferas en las DOS camaras (AND) y la camara " +
                         std::to_string(bad->camera) + " midio " +
                         std::to_string(bad->spheres) + " (" + verdict.detalle + ")";
    }
    return verdict;
}

} // namespace ArmLock
