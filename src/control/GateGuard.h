/**
 * GateGuard.h
 *
 * Guarda de la compuerta: la carrera NO se larga si las 10 esferas no estan
 * arriba. Es la tercera pata del reparto arm/start:
 *
 *   /arm   -> "la pista esta lista": ESP32, VideoHub, vMix, DeckLink y las 10
 *             esferas abajo en la META (llegada), dos camaras AND.
 *   /start -> streaming + coreografia.
 *   guion  -> reload -> suben -> ESTA guarda (¿10 arriba?) -> abre la compuerta
 *                                └─ si no -> NO abre y avisa
 *
 * Vive afuera del motor de coreografia y del medidor a proposito: sin Windows,
 * sin hilos y sin reloj real. Quien llama aporta tres verbos (sondear el
 * medidor, esperar y leer el reloj), asi la decision se puede probar de verdad
 * en un test de host (tests/test_gate_guard.cpp) y no solo leyendo el codigo.
 *
 * El caso que importa: con 9 arriba la compuerta NO se abre y el motivo dice
 * cuantas se midieron. Un "no pude verificar" tampoco abre.
 */

#pragma once

#include <cstdint>
#include <string>

namespace GateGuard {

/** Lo ultimo que termino de medir el medidor por evento (una medicion, no una sonda). */
struct Medida {
    bool hay = false;          // false = el medidor todavia no termino ninguna
    int punto = 0;             // punto del guion (1..8)
    int esferas = 0;           // posiciones resueltas
    int64_t terminadaMs = 0;   // epoch ms en que el hilo medidor termino el trabajo
};

/** Veredicto de la guarda, listo para el log y para el error del guion. */
struct Veredicto {
    bool ok = false;
    std::string motivo;
};

/**
 * Decision pura: ¿la compuerta puede abrirse con esta medicion?
 *
 * @param medida           medicion terminada (se ignora si !hay)
 * @param puntoExigido     punto del guion que verifica la salida (produccion: 1)
 * @param esferasExigidas  esferas que tienen que estar arriba (produccion: 10).
 *                         0 o menos = sin guarda (comportamiento de siempre)
 */
inline Veredicto Decidir(const Medida& medida, int puntoExigido, int esferasExigidas) {
    Veredicto veredicto;

    if (esferasExigidas <= 0) {
        veredicto.ok = true;
        veredicto.motivo = "sin guarda de esferas: la compuerta abre como siempre";
        return veredicto;
    }
    if (!medida.hay) {
        veredicto.ok = false;
        veredicto.motivo = "no hay medicion del punto " + std::to_string(puntoExigido) +
                           ": no se puede verificar que las esferas hayan llegado";
        return veredicto;
    }
    if (medida.punto != puntoExigido) {
        veredicto.ok = false;
        veredicto.motivo = "la ultima medicion es del punto " + std::to_string(medida.punto) +
                           " y la guarda exige el punto " + std::to_string(puntoExigido) + ": no se abre";
        return veredicto;
    }
    if (medida.esferas != esferasExigidas) {
        veredicto.ok = false;
        veredicto.motivo = "arriba se midieron " + std::to_string(medida.esferas) +
                           " esferas en el punto " + std::to_string(puntoExigido) +
                           " y se exigen " + std::to_string(esferasExigidas) +
                           ": NO se abre la compuerta";
        return veredicto;
    }

    veredicto.ok = true;
    veredicto.motivo = "arriba estan las " + std::to_string(medida.esferas) +
                       " esferas (punto " + std::to_string(puntoExigido) + "): la compuerta puede abrir";
    return veredicto;
}

/**
 * Espera a que el medidor TERMINE el punto exigido y decide.
 *
 * La medicion se encola en el guion (MeasurePoint) y la resuelve el hilo
 * medidor: cuando la guarda corre puede no haber terminado todavia (la rafaga
 * dura varios segundos). Se sondea hasta que aparezca una medicion del punto
 * terminada DESPUES de sinceMs (asi no vale la de la carrera anterior).
 *
 * @param punto            punto del guion a verificar
 * @param esferasExigidas  esferas exigidas (0 o menos = sin guarda)
 * @param timeoutMs        tope de espera; al vencer NO se abre
 * @param sinceMs          instante de arranque del guion (epoch ms)
 * @param sondear          () -> bool, deja en una Medida lo ultimo terminado
 * @param esperar          (ms) -> void, duerme un tramo
 * @param reloj            () -> int64_t, epoch ms
 */
template <typename SondeoFn, typename EsperaFn, typename RelojFn>
Veredicto Esperar(int punto, int esferasExigidas, int timeoutMs, int64_t sinceMs,
                  SondeoFn&& sondear, EsperaFn&& esperar, RelojFn&& reloj) {
    if (esferasExigidas <= 0) {
        return Decidir(Medida{}, punto, esferasExigidas);
    }

    const int64_t limite = reloj() + ((timeoutMs > 0) ? timeoutMs : 10000);
    for (;;) {
        Medida medida;
        if (sondear(medida) && medida.hay && medida.terminadaMs >= sinceMs) {
            // La medicion del punto puede no ser la ultima terminada (un punto
            // posterior ya resuelto, por ejemplo): se exige que sea la del punto.
            if (medida.punto == punto) {
                return Decidir(medida, punto, esferasExigidas);
            }
        }

        const int64_t ahora = reloj();
        if (ahora >= limite) {
            break;
        }
        const int64_t restante = limite - ahora;
        esperar(static_cast<int>((restante < 100) ? restante : 100));
    }

    Veredicto veredicto;
    veredicto.ok = false;
    veredicto.motivo = "no se termino de medir el punto " + std::to_string(punto) + " en " +
                       std::to_string((timeoutMs > 0) ? timeoutMs : 10000) +
                       " ms: NO se abre la compuerta (no se pudo verificar que las esferas "
                       "hayan llegado arriba)";
    return veredicto;
}

} // namespace GateGuard
