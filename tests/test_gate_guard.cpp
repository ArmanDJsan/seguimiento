/**
 * test_gate_guard.cpp
 *
 * Test de host (Linux, sin Windows ni CUDA) de la guarda de la compuerta.
 * Compila y corre con:
 *   g++ -std=c++20 -Wall -Wextra -Werror tests/test_gate_guard.cpp -o /tmp/test_gate_guard
 *   /tmp/test_gate_guard
 *
 * Lo que se prueba aca es lo que decide, dentro del guion, si la compuerta
 * abre o no: con 9 arriba NO abre, sin medicion NO abre, y con 10 abre. El
 * reloj y el sondeo son falsos, asi que cada caso corre en microsegundos.
 */

#include "../src/control/GateGuard.h"

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

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

GateGuard::Medida MedidaDe(int punto, int esferas, int64_t terminadaMs) {
    GateGuard::Medida medida;
    medida.hay = true;
    medida.punto = punto;
    medida.esferas = esferas;
    medida.terminadaMs = terminadaMs;
    return medida;
}

// Guion: la medicion del punto aparece recien en 'apareceMs' (reloj falso) con
// 'esferas'. Devuelve el veredicto y cuantas veces se sono el medidor.
struct Escenario {
    int puntoQueAparece = 1;
    int esferas = 10;
    int64_t apareceMs = 1500;
    // Medicion vieja (de la carrera anterior) que ya estaba antes de arrancar.
    bool hayMedicionVieja = false;
};

GateGuard::Veredicto Correr(const Escenario& esc, int* sondeos = nullptr) {
    int64_t ahora = 1000;   // el guion arranco en 1000
    const int64_t since = ahora;
    int intentos = 0;

    auto sondear = [&](GateGuard::Medida& medida) {
        ++intentos;
        if (esc.hayMedicionVieja) {
            // Terminada ANTES del arranque: no vale para esta carrera.
            medida = MedidaDe(esc.puntoQueAparece, 10, 900);
            return true;
        }
        if (ahora >= esc.apareceMs + 1000) {
            medida = MedidaDe(esc.puntoQueAparece, esc.esferas, ahora);
            return true;
        }
        medida = GateGuard::Medida{};
        return false;
    };
    auto esperar = [&](int ms) { ahora += ms; };
    auto reloj = [&]() { return ahora; };

    const GateGuard::Veredicto veredicto =
        GateGuard::Esperar(1, 10, 5000, since, sondear, esperar, reloj);
    if (sondeos) {
        *sondeos = intentos;
    }
    return veredicto;
}

} // namespace

int main() {
    // --- Decision pura -----------------------------------------------------
    {
        const auto veredicto = GateGuard::Decidir(MedidaDe(1, 10, 2000), 1, 10);
        Check(veredicto.ok, "10 arriba: la compuerta abre");
        Check(Contains(veredicto.motivo, "10"), "el motivo dice cuantas se midieron");
    }
    {
        const auto veredicto = GateGuard::Decidir(MedidaDe(1, 9, 2000), 1, 10);
        Check(!veredicto.ok, "9 arriba: la compuerta NO abre");
        Check(Contains(veredicto.motivo, "9"), "el motivo dice que se midieron 9");
        Check(Contains(veredicto.motivo, "NO se abre"), "el motivo dice que no se abre");
    }
    {
        const auto veredicto = GateGuard::Decidir(GateGuard::Medida{}, 1, 10);
        Check(!veredicto.ok, "sin medicion: la compuerta NO abre");
    }
    {
        const auto veredicto = GateGuard::Decidir(MedidaDe(2, 10, 2000), 1, 10);
        Check(!veredicto.ok, "medicion de otro punto: la compuerta NO abre");
    }
    {
        const auto veredicto = GateGuard::Decidir(GateGuard::Medida{}, 1, 0);
        Check(veredicto.ok, "sin guarda (require_spheres=0): abre como siempre");
    }

    // --- Espera con reloj falso -------------------------------------------
    {
        Escenario esc;
        esc.apareceMs = 1500;
        int sondeos = 0;
        const auto veredicto = Correr(esc, &sondeos);
        Check(veredicto.ok, "la medicion llega tarde: la guarda espera y abre con 10");
        Check(sondeos > 1, "la guarda sono el medidor mas de una vez");
    }
    {
        Escenario esc;
        esc.esferas = 9;
        const auto veredicto = Correr(esc);
        Check(!veredicto.ok, "la medicion llega con 9: la compuerta NO abre");
        Check(Contains(veredicto.motivo, "9"), "el motivo del 9 llega al guion");
    }
    {
        Escenario esc;
        esc.apareceMs = 60000;   // nunca dentro del timeout
        const auto veredicto = Correr(esc);
        Check(!veredicto.ok, "sin medicion dentro del timeout: la compuerta NO abre");
        Check(Contains(veredicto.motivo, "no se termino de medir"),
              "el motivo dice que no se pudo verificar");
    }
    {
        Escenario esc;
        esc.hayMedicionVieja = true;
        const auto veredicto = Correr(esc);
        Check(!veredicto.ok, "una medicion de la carrera anterior no vale: NO abre");
    }
    {
        // Primero aparece el punto 2 (no es el exigido) y despues el 1.
        int64_t ahora = 1000;
        const int64_t since = ahora;
        auto sondear = [&](GateGuard::Medida& medida) {
            if (ahora >= 2500) {
                medida = MedidaDe(1, 10, ahora);
                return true;
            }
            if (ahora >= 1500) {
                medida = MedidaDe(2, 10, ahora);
                return true;
            }
            medida = GateGuard::Medida{};
            return false;
        };
        auto esperar = [&](int ms) { ahora += ms; };
        auto reloj = [&]() { return ahora; };
        const auto veredicto = GateGuard::Esperar(1, 10, 5000, since, sondear, esperar, reloj);
        Check(veredicto.ok, "un punto posterior no tapa al exigido: sigue esperando y abre");
    }

    if (g_fallos == 0) {
        std::printf("\nTODOS LOS CHEQUEOS PASARON\n");
        return 0;
    }
    std::printf("\n%d CHEQUEOS FALLARON\n", g_fallos);
    return 1;
}
