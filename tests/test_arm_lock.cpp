/**
 * test_arm_lock.cpp
 *
 * Test de host (Linux, sin Windows ni CUDA) de la regla AND del armado.
 * Compila y corre con:
 *   g++ -std=c++20 -Wall -Wextra -Werror tests/test_arm_lock.cpp -o /tmp/test_arm_lock
 *   /tmp/test_arm_lock
 *
 * Lo que se prueba aca es lo que en el VIB corre dentro del handler /arm:
 * el orden (encuadrar TODAS -> medir TODAS), que se midan las dos aunque la
 * primera falle, y que el veredicto exija el mismo conteo en las dos.
 */

#include "../src/control/ArmLock.h"

#include <cstdio>
#include <string>
#include <vector>

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

std::vector<ArmLock::CameraSpec> DosCamaras() {
    return {{4, 2}, {3, 3}};
}

// Escenario: las dos camaras miden lo que dice 'spheres'; se registra el orden
// de las llamadas para verificar que primero se encuadra y despues se mide.
ArmLock::Verdict Correr(const std::vector<int>& spheres,
                        std::vector<std::string>* orden = nullptr,
                        const std::string& moverFalla = std::string(),
                        int sondaQueNoMide = 0) {
    std::vector<ArmLock::CameraSpec> cameras = DosCamaras();
    size_t probeIndex = 0;

    auto move = [&](int camera, int preset, std::string& motivo) {
        if (orden) {
            orden->push_back("mover " + std::to_string(camera) + "/" + std::to_string(preset));
        }
        if (camera == 4 && moverFalla == "cam4") {
            motivo = "el PTZ rechazo el preset 2 de la camara 4";
            return false;
        }
        return true;
    };

    auto probe = [&](int camera, int preset, int& out, std::string& motivo) {
        (void)preset;
        if (orden) {
            orden->push_back("medir " + std::to_string(camera));
        }
        // Una entrada de 'spheres' por camara, en el orden del lock: que una
        // sonda no devuelva resultado no corre el conteo de la siguiente.
        out = (probeIndex < spheres.size()) ? spheres[probeIndex] : 0;
        ++probeIndex;
        if (camera == sondaQueNoMide) {
            out = 0;
            motivo = "sin resultado de la sonda en 20000 ms";
            return false;
        }
        return true;
    };

    return ArmLock::Run(cameras, 10, move, probe);
}

void TestAndOk() {
    const auto verdict = Correr({10, 10});
    Check(verdict.ok, "10 y 10 -> lock OK");
    Check(verdict.readings.size() == 2, "10 y 10 -> dos lecturas");
    Check(verdict.readings[0].camera == 4 && verdict.readings[0].spheres == 10,
          "10 y 10 -> la lectura de la camara 4 queda registrada");
    Check(verdict.readings[1].camera == 3 && verdict.readings[1].spheres == 10,
          "10 y 10 -> la lectura de la camara 3 queda registrada");
    Check(Contains(verdict.detalle, "cam 4 = 10") && Contains(verdict.detalle, "cam 3 = 10"),
          "10 y 10 -> el detalle nombra las dos camaras");
}

void TestAndFallaSegunda() {
    const auto verdict = Correr({10, 9});
    Check(!verdict.ok, "10 y 9 -> lock rechazado");
    Check(verdict.failingCamera == 3 && verdict.failingSpheres == 9,
          "10 y 9 -> el 409 apunta a la camara 3 con 9 esferas");
    Check(Contains(verdict.motivo, "DOS"), "10 y 9 -> el motivo dice que se exigen las DOS");
    Check(Contains(verdict.motivo, "cam 4 = 10"), "10 y 9 -> el motivo conserva el 10 de la otra camara");
    Check(verdict.readings.size() == 2, "10 y 9 -> se midieron las dos");
}

void TestAndFallaPrimera() {
    const auto verdict = Correr({8, 10});
    Check(!verdict.ok, "8 y 10 -> lock rechazado");
    Check(verdict.failingCamera == 4 && verdict.failingSpheres == 8,
          "8 y 10 -> el 409 apunta a la camara 4 con 8 esferas");
    Check(verdict.readings[1].spheres == 10, "8 y 10 -> la segunda camara igual se midio");
}

void TestExigeExacto() {
    const auto verdict = Correr({11, 10});
    Check(!verdict.ok && verdict.failingCamera == 4 && verdict.failingSpheres == 11,
          "11 y 10 -> 11 no es 10: se rechaza igual");
}

void TestSondaSinResultadoNoCortaLaOtra() {
    const auto verdict = Correr({10, 10}, nullptr, std::string(), /*sondaQueNoMide=*/4);
    Check(!verdict.ok, "sonda sin resultado -> lock rechazado");
    Check(verdict.failingCamera == 4 && verdict.failingSpheres == 0,
          "sonda sin resultado -> el 409 apunta a la camara que no midio");
    Check(verdict.readings.size() == 2 && verdict.readings[1].measured,
          "sonda sin resultado -> la otra camara se mide igual");
    Check(Contains(verdict.motivo, "no se pudo medir") && Contains(verdict.motivo, "sin resultado"),
          "sonda sin resultado -> el motivo explica que no hubo medicion");
}

void TestPtzRechazadoNoMide() {
    std::vector<std::string> orden;
    const auto verdict = Correr({10, 10}, &orden, "cam4");
    Check(!verdict.ok && verdict.failingCamera == 4,
          "PTZ rechazado -> el 409 apunta a la camara 4");
    Check(Contains(verdict.motivo, "PTZ rechazo"), "PTZ rechazado -> el motivo conserva el error del VISCA");
    bool seMidio = false;
    for (const auto& paso : orden) {
        seMidio = seMidio || (paso.rfind("medir", 0) == 0);
    }
    Check(!seMidio, "PTZ rechazado -> no se mide nada (el encuadre no esta verificado)");
}

void TestOrdenEncuadrarLuegoMedir() {
    std::vector<std::string> orden;
    const auto verdict = Correr({10, 10}, &orden);
    Check(verdict.ok, "orden -> el lock pasa");
    const std::vector<std::string> esperado = {"mover 4/2", "mover 3/3", "medir 4", "medir 3"};
    Check(orden == esperado, "orden -> primero se encuadran LAS DOS y despues se mide");
}

void TestSinCamaras() {
    std::vector<ArmLock::CameraSpec> vacio;
    const auto verdict = ArmLock::Run(vacio, 10,
        [](int, int, std::string&) { return true; },
        [](int, int, int&, std::string&) { return true; });
    Check(!verdict.ok && Contains(verdict.motivo, "no tiene camaras"),
          "lock sin camaras -> rechazado con motivo");
}

} // namespace

int main() {
    TestAndOk();
    TestAndFallaSegunda();
    TestAndFallaPrimera();
    TestExigeExacto();
    TestSondaSinResultadoNoCortaLaOtra();
    TestPtzRechazadoNoMide();
    TestOrdenEncuadrarLuegoMedir();
    TestSinCamaras();

    if (g_fallos != 0) {
        std::printf("\n%d FALLOS\n", g_fallos);
        return 1;
    }
    std::printf("\nTODOS LOS TESTS PASARON\n");
    return 0;
}
