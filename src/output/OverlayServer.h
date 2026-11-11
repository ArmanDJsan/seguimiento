/**
 * OverlayServer.h
 *
 * Servidor HTTP local que sirve el overlay de la carrera (HTML/JS) y el estado
 * del ultimo ranking. Reemplaza al programa Go externo (metra_overlay_renderer)
 * y, mas adelante, a la animacion de TouchDesigner: vMix captura esta URL como
 * "Web Browser" y no hay un tercer programa en el medio.
 *
 * Contrato con la web (identico al que ya consumia el receptor UDP):
 *   {"type":"ranking","seq":N,"canicas":[{"id":0,"puesto":3},...]}
 *   - id 0 = canica indeterminada
 *   - siempre 10 canicas, puestos 1..10
 *
 * Endpoints (bind SOLO en loopback: vMix corre en esta misma maquina):
 *   GET /            -> overlay/index.html
 *   GET /app.js, /style.css, /assets/*  -> estaticos del directorio del overlay
 *   GET /api/state   -> {"version":N,"updated_ms":<epoch>,"payload":{...}|null}
 *   GET /healthz     -> "ok"
 *
 * Thread safety: Publish() es seguro desde cualquier hilo (el medidor publica).
 * El servidor corre en su propio hilo; no toca el hilo de captura.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httplib { class Server; }

/** Configuracion del overlay (bloque "overlay" de config.json) */
struct OverlayConfig {
    bool enabled = false;
    uint16_t port = 8091;
    std::string dir = "overlay";   ///< carpeta con index.html, app.js, assets/
};

class OverlayServer {
public:
    OverlayServer();
    ~OverlayServer();

    OverlayServer(const OverlayServer&) = delete;
    OverlayServer& operator=(const OverlayServer&) = delete;

    /** Levanta el servidor si enabled. Devuelve false si no pudo escuchar. */
    bool Start(const OverlayConfig& config);

    /** Corta el servidor y espera al hilo. */
    void Stop();

    /** Guarda el ultimo payload publicado y (etapa 2) lo empuja por SSE. */
    void Publish(const std::string& payload);

    bool Running() const { return m_running.load(); }

private:
    OverlayConfig m_config;
    std::unique_ptr<httplib::Server> m_server;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    mutable std::mutex m_mutex;
    std::string m_lastPayload;      ///< ultimo JSON publicado (vacio = todavia nada)
    int64_t m_lastMs = 0;
    uint64_t m_version = 0;
};
