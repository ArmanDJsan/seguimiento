/**
 * PistaReadiness.h
 *
 * "¿La pista esta lista para una carrera?" — los chequeos del /arm.
 *
 * El /arm ya no arma nada (no manda el reload ni toca las camaras de la
 * salida): verifica que TODO lo que tiene que estar, este, y recien despues
 * cuenta las 10 esferas abajo en la META. Si algo falta, el 409 dice que.
 *
 * Los cuatro chequeos son los mismos de la Fase 1 de la ignicion (menu [2]),
 * pero bajo pedido y con veredicto por item:
 *   1. ESP32 responde (/status).
 *   2. VideoHub: acepta el ruteo de los 16 puertos (Streaming 1-12 y
 *      Seguimiento 13-16) por la salida de monitoreo del VIB.
 *   3. vMix: la API responde y no hay inputs offline.
 *   4. DeckLink: los canales de captura estan ENTRANDO cuadros (contador real,
 *      no el stub de senal del SDK).
 *
 * Corre en el hilo HTTP del /arm: usa los mismos clientes que el pipeline
 * (VideoHub, vMix, ESP32) y solo LEE el contador de frames de la captura; no
 * toca los buffers ni el zero-copy.
 */

#pragma once

#include <string>
#include <vector>

class VideoHubClient;
class DeckLinkSource;
class VMixController;
class TrackPhysicalController;
class DeckLinkCapture;

namespace Pista {

/** Salida del VideoHub que usa el VIB para el barrido de senales (main.cpp). */
constexpr int kSalidaMonitoreo = 0;

/** Handles del pipeline que corren. nullptr = ese chequeo no se puede hacer. */
struct Fuente {
    VideoHubClient* videoHub = nullptr;
    DeckLinkSource* deckLink = nullptr;
    VMixController* vmix = nullptr;
    TrackPhysicalController* esp32 = nullptr;
    // Canales de captura vivos (el vector lo duenia main.cpp). nullptr o vacio
    // = no hay canales que verificar.
    const std::vector<DeckLinkCapture*>* captura = nullptr;
};

/** Un chequeo, con lo que se verifico o por que fallo. */
struct Chequeo {
    std::string id;       // "esp32" | "videohub" | "vmix" | "decklink"
    std::string nombre;   // "ESP32 responde"
    bool ok = false;
    std::string detalle;  // que se midio / por que fallo
};

/** Veredicto del chequeo de pista. */
struct Veredicto {
    bool ok = false;
    std::vector<Chequeo> chequeos;
    std::string motivo;   // resumen en una frase para el 409
};

struct Opciones {
    // Puertos del barrido (1-indexed, como el VideoHub y la Fase 1).
    std::vector<int> puertosStreaming{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    std::vector<int> puertosSeguimiento{13, 14, 15, 16};
    bool barridoVideoHub = true;   // false = solo mira que el VideoHub este conectado
    // Ventana para ver avanzar los contadores de frames de la captura.
    int ventanaDeckLinkMs = 500;
    // false = no exige canales de captura (la ignicion corre ANTES de crearlos;
    // ahi este chequeo todavia no tiene nada que mirar).
    bool checkDeckLink = true;
    // El barrido deja la salida de monitoreo apuntando al ultimo puerto. Al
    // terminar se la devuelve a CAM_01, que es como la deja la Fase 2 en la
    // ignicion: /arm no puede cambiarle el ruteo a la operacion.
    bool restaurarSalidaMonitoreo = true;
};

/**
 * Corre los cuatro chequeos. No corta en el primero que falla: el 409 tiene
 * que decir todo lo que esta mal, no solo lo primero que se encontro.
 */
Veredicto Chequear(const Fuente& fuente, const Opciones& opciones);

/** Resumen en una linea para el log. */
std::string Resumen(const Veredicto& veredicto);

/** Fragmento JSON del array "chequeos":[{...}] (para el 200 y el 409). */
std::string ChequeosJson(const Veredicto& veredicto);

} // namespace Pista
