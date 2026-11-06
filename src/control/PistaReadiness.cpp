/**
 * PistaReadiness.cpp
 *
 * Implementacion de los chequeos de pista del /arm. Ver PistaReadiness.h.
 *
 * Nada de esto mueve la pista: el barrido del VideoHub cambia el ruteo de la
 * salida de MONITOREO del VIB (la que usa la Fase 1 para ver senales), no las
 * salidas que alimentan a vMix; el ESP32 se consulta con /status (no /test);
 * y de la captura solo se leen contadores.
 */

// vMix primero: define NOMINMAX e incluye winsock2 antes de windows.h.
#include "VMixController.h"

#include "PistaReadiness.h"

#include "TrackPhysicalController.h"
#include "VideoHubClient.h"
#include "../capture/DeckLinkCapture.h"
#include "../capture/DeckLinkSource.h"
#include "../utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace {

Pista::Chequeo& Agregar(std::vector<Pista::Chequeo>& chequeos,
                        const std::string& id, const std::string& nombre) {
    chequeos.push_back(Pista::Chequeo{id, nombre, false, std::string()});
    return chequeos.back();
}

/**
 * Barrido de un grupo de puertos: rutea cada uno por la salida de monitoreo y
 * consulta la senal de las entradas del DeckLink.
 *
 * @param detalle [out] que se verifico (cuando sale bien)
 * @param fallo   [out] que puerto fallo y por que
 */
bool Barrido(VideoHubClient& videoHub, DeckLinkSource& deckLink,
             const std::vector<int>& puertos, const std::string& etiqueta,
             std::string& detalle, std::string& fallo) {
    for (int puerto : puertos) {
        // El VideoHub numera los puertos desde 0; el guion y el config, desde 1.
        const int entrada = puerto - 1;
        if (!videoHub.RouteInputToOutput(Pista::kSalidaMonitoreo, entrada)) {
            fallo = "el VideoHub no acepto el ruteo del puerto " + std::to_string(puerto) +
                    " (" + etiqueta + ")";
            return false;
        }
    }

    // El switch tarda en asentar el ruteo: sin esta pausa se consulta la senal
    // del puerto anterior.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto estados = deckLink.GetSignalStatus(puertos);
    for (const auto& estado : estados) {
        if (!estado.signalLocked) {
            fallo = "sin senal en el puerto " + std::to_string(estado.index) +
                    " (" + estado.name.value_or("sin nombre") + ") del barrido " + etiqueta;
            return false;
        }
    }

    detalle = etiqueta + ": " + std::to_string(puertos.size()) +
              " puertos ruteados y con senal";
    return true;
}

} // namespace

namespace Pista {

Veredicto Chequear(const Fuente& fuente, const Opciones& opciones) {
    Veredicto veredicto;

    // ------------------------------------------------------------------
    // 1. ESP32: responde /status. NO se manda /test: ese comando mueve la
    //    mecanica y el /arm no toca la pista.
    // ------------------------------------------------------------------
    {
        Chequeo& chequeo = Agregar(veredicto.chequeos, "esp32", "ESP32 responde");
        if (!fuente.esp32) {
            chequeo.detalle = "sin controlador del ESP32 en este modo";
        } else if (fuente.esp32->executeTest()) {
            chequeo.ok = true;
            chequeo.detalle = "el ESP32 contesto /status";
        } else {
            chequeo.detalle = "el ESP32 no contesto /status: revisar su red y su alimentacion";
        }
    }

    // ------------------------------------------------------------------
    // 2. VideoHub: barrido de Streaming (1-12) y Seguimiento (13-16).
    // ------------------------------------------------------------------
    {
        Chequeo& chequeo = Agregar(veredicto.chequeos, "videohub", "VideoHub y senales");
        if (!fuente.videoHub) {
            chequeo.detalle = "sin cliente del VideoHub en este modo";
        } else if (!fuente.videoHub->IsConnected()) {
            chequeo.detalle = "el VideoHub no esta conectado";
        } else if (!opciones.barridoVideoHub) {
            chequeo.ok = true;
            chequeo.detalle = "el VideoHub esta conectado (barrido desactivado en config)";
        } else if (!fuente.deckLink) {
            chequeo.detalle = "sin consulta de senal del DeckLink: no se puede barrer";
        } else {
            std::string detalleStreaming, detalleSeguimiento, fallo;
            if (!Barrido(*fuente.videoHub, *fuente.deckLink, opciones.puertosStreaming,
                         "Streaming (1-12)", detalleStreaming, fallo)) {
                chequeo.detalle = fallo;
            } else if (!Barrido(*fuente.videoHub, *fuente.deckLink, opciones.puertosSeguimiento,
                                "Seguimiento (13-16)", detalleSeguimiento, fallo)) {
                chequeo.detalle = fallo;
            } else {
                chequeo.ok = true;
                chequeo.detalle = detalleStreaming + "; " + detalleSeguimiento;
                // El barrido dejo la salida de monitoreo en el ultimo puerto.
                // Se la devuelve a CAM_01 (como la deja la Fase 2) para no
                // cambiarle el ruteo a la operacion desde el /arm.
                if (opciones.restaurarSalidaMonitoreo) {
                    if (fuente.videoHub->RouteInputToOutput(kSalidaMonitoreo, "CAM_01")) {
                        chequeo.detalle += "; salida de monitoreo devuelta a CAM_01";
                    } else {
                        Logger::Warning("[API] /arm: el barrido dejo la salida de monitoreo "
                                        "fuera de CAM_01 y no se pudo restaurar");
                        chequeo.detalle += "; OJO: no se pudo devolver la salida de monitoreo "
                                           "a CAM_01";
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // 3. vMix: la API responde y no hay inputs offline.
    // ------------------------------------------------------------------
    {
        Chequeo& chequeo = Agregar(veredicto.chequeos, "vmix", "vMix con inputs sanos");
        if (!fuente.vmix) {
            chequeo.detalle = "sin cliente de vMix en este modo";
        } else if (fuente.vmix->CheckInputsHealthy()) {
            chequeo.ok = true;
            chequeo.detalle = "la API de vMix responde y no hay inputs offline";
        } else {
            chequeo.detalle = "vMix no responde o tiene inputs offline (ver el log)";
        }
    }

    // ------------------------------------------------------------------
    // 4. DeckLink: los canales de captura estan entrando cuadros. Es la unica
    //    prueba real de senal: la consulta de lock del SDK es un stub.
    // ------------------------------------------------------------------
    if (opciones.checkDeckLink) {
        Chequeo& chequeo = Agregar(veredicto.chequeos, "decklink", "DeckLink capturando");
        if (!fuente.captura || fuente.captura->empty()) {
            chequeo.detalle = "no hay canales de captura inicializados";
        } else {
            std::vector<uint64_t> antes;
            antes.reserve(fuente.captura->size());
            for (const DeckLinkCapture* canal : *fuente.captura) {
                antes.push_back(canal ? canal->FramesArrived() : 0);
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds((opciones.ventanaDeckLinkMs > 0)
                                              ? opciones.ventanaDeckLinkMs
                                              : 500));

            std::ostringstream detalle;
            std::string sinCuadros;
            size_t conCuadros = 0;
            for (size_t i = 0; i < fuente.captura->size(); ++i) {
                const DeckLinkCapture* canal = (*fuente.captura)[i];
                const uint64_t despues = canal ? canal->FramesArrived() : 0;
                const uint64_t nuevos = (despues >= antes[i]) ? (despues - antes[i]) : 0;
                const std::string nombre =
                    canal ? canal->GetChannel().channelName : ("canal " + std::to_string(i + 1));
                if (nuevos > 0) {
                    ++conCuadros;
                    if (conCuadros > 1) {
                        detalle << ", ";
                    }
                    detalle << nombre << " +" << nuevos << " cuadros";
                } else if (sinCuadros.empty()) {
                    sinCuadros = nombre;
                } else {
                    sinCuadros += ", " + nombre;
                }
            }

            if (!sinCuadros.empty()) {
                chequeo.detalle = "sin cuadros en " + std::to_string(opciones.ventanaDeckLinkMs) +
                                  " ms en: " + sinCuadros;
            } else {
                chequeo.ok = true;
                chequeo.detalle = detalle.str();
            }
        }
    }

    // Veredicto: ok solo si estan todos. El motivo nombra los que fallaron.
    veredicto.ok = std::all_of(veredicto.chequeos.begin(), veredicto.chequeos.end(),
                               [](const Chequeo& chequeo) { return chequeo.ok; });
    if (!veredicto.ok) {
        std::ostringstream motivo;
        motivo << "la pista no esta lista: ";
        bool primero = true;
        for (const auto& chequeo : veredicto.chequeos) {
            if (chequeo.ok) {
                continue;
            }
            if (!primero) {
                motivo << "; ";
            }
            motivo << chequeo.nombre << " (" << chequeo.detalle << ")";
            primero = false;
        }
        veredicto.motivo = motivo.str();
    } else {
        veredicto.motivo = "pista lista: " + std::to_string(veredicto.chequeos.size()) +
                           " chequeos en orden";
    }

    return veredicto;
}

std::string Resumen(const Veredicto& veredicto) {
    std::ostringstream oss;
    for (size_t i = 0; i < veredicto.chequeos.size(); ++i) {
        const Chequeo& chequeo = veredicto.chequeos[i];
        if (i > 0) {
            oss << " | ";
        }
        oss << chequeo.id << "=" << (chequeo.ok ? "OK" : "FALLA");
    }
    return oss.str();
}

std::string ChequeosJson(const Veredicto& veredicto) {
    // Escapes minimos: los detalles son ASCII (nombres de canal y puertos).
    auto escapar = [](const std::string& texto) {
        std::string salida;
        salida.reserve(texto.size() + 8);
        for (char c : texto) {
            switch (c) {
                case '"':  salida += "\\\""; break;
                case '\\': salida += "\\\\"; break;
                case '\n': salida += "\\n"; break;
                case '\r': salida += "\\r"; break;
                case '\t': salida += "\\t"; break;
                default:   salida += c; break;
            }
        }
        return salida;
    };

    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < veredicto.chequeos.size(); ++i) {
        const Chequeo& chequeo = veredicto.chequeos[i];
        if (i > 0) {
            oss << ",";
        }
        oss << "{\"id\":\"" << escapar(chequeo.id) << "\""
            << ",\"ok\":" << (chequeo.ok ? "true" : "false")
            << ",\"detalle\":\"" << escapar(chequeo.detalle) << "\"}";
    }
    oss << "]";
    return oss.str();
}

} // namespace Pista
