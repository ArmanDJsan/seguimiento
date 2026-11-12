/**
 * OverlayServer.cpp
 *
 * Servidor: cpp-httplib (single header, MIT) en third_party/httplib.h, el mismo
 * que ya usa la API de control. Se elige httplib por la misma razon: no hay que
 * escribir un servidor HTTP a mano y el proyecto ya linkea Ws2_32.
 *
 * Decisiones:
 *  - Bind a 127.0.0.1: el unico consumidor es el navegador de vMix, que corre en
 *    esta maquina. No se expone a la red ni necesita X-Api-Key.
 *  - Los estaticos salen del directorio `overlay\` que viaja al lado del exe
 *    (asi el dueno puede editar el HTML/CSS sin recompilar).
 *  - Publish() solo guarda el ultimo payload y su version: la web lo consulta con
 *    /api/state. El empuje por SSE (/events) es la etapa 2.
 */

#include "OverlayServer.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "../../third_party/httplib.h"
#include "../utils/Logger.h"

namespace {

int64_t NowEpochMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

bool LeerArchivo(const std::string& ruta, std::string* out) {
    std::ifstream in(ruta, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

std::string TipoDe(const std::string& ruta) {
    if (ruta.size() >= 5 && ruta.compare(ruta.size() - 5, 5, ".html") == 0) return "text/html; charset=utf-8";
    if (ruta.size() >= 3 && ruta.compare(ruta.size() - 3, 3, ".js") == 0) return "application/javascript; charset=utf-8";
    if (ruta.size() >= 4 && ruta.compare(ruta.size() - 4, 4, ".css") == 0) return "text/css; charset=utf-8";
    if (ruta.size() >= 4 && ruta.compare(ruta.size() - 4, 4, ".png") == 0) return "image/png";
    if (ruta.size() >= 4 && ruta.compare(ruta.size() - 4, 4, ".svg") == 0) return "image/svg+xml";
    if (ruta.size() >= 4 && ruta.compare(ruta.size() - 4, 4, ".ico") == 0) return "image/x-icon";
    return "application/octet-stream";
}

}  // namespace

OverlayServer::OverlayServer() = default;

OverlayServer::~OverlayServer() {
    Stop();
}

bool OverlayServer::Start(const OverlayConfig& config) {
    m_config = config;
    if (!m_config.enabled) {
        Logger::Info("OverlayServer: deshabilitado por configuracion");
        return false;
    }

    m_server = std::make_unique<httplib::Server>();

    // Estado del ultimo ranking. `payload` va como objeto JSON crudo.
    m_server->Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
        std::string payload;
        int64_t ms = 0;
        uint64_t version = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            payload = m_lastPayload;
            ms = m_lastMs;
            version = m_version;
        }
        std::ostringstream oss;
        oss << "{\"version\":" << version << ",\"updated_ms\":" << ms << ",\"payload\":";
        oss << (payload.empty() ? std::string("null") : payload);
        oss << "}";
        res.set_content(oss.str(), "application/json; charset=utf-8");
        res.set_header("Cache-Control", "no-store");
    });

    // SSE: la web escucha events y recibe cada ranking en el momento en que se
    // publica. Es lo que reemplaza al UDP de TouchDesigner.
    m_server->Get("/events", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache, no-transform");
        res.set_header("X-Accel-Buffering", "no");
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_chunked_content_provider(
            "text/event-stream",
            [this](size_t, httplib::DataSink& sink) {
                ++m_clients;
                struct GuardiaClientes {
                    std::atomic<int>* n;
                    ~GuardiaClientes() { --(*n); }
                } guardia{&m_clients};
                uint64_t visto = 0;
                std::string payload;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    visto = m_version;
                    payload = m_lastPayload;
                }
                // Al conectarse, el estado actual (si hay) va de una.
                if (!payload.empty()) {
                    const std::string msg = "event: ranking\ndata: " + payload + "\n\n";
                    if (!sink.write(msg.data(), msg.size())) return false;
                }
                while (sink.is_writable()) {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait_for(lock, std::chrono::seconds(10),
                                  [this, visto] { return m_version != visto; });
                    visto = m_version;
                    payload = m_lastPayload;
                    lock.unlock();
                    const std::string msg = payload.empty()
                        ? std::string(": ping\n\n")
                        : ("event: ranking\ndata: " + payload + "\n\n");
                    if (!sink.write(msg.data(), msg.size())) return false;
                }
                return false;
            });
    });

    m_server->Get("/healthz", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok", "text/plain");
    });

    // Prueba sin carrera: se le manda el mismo JSON que publica el medidor y el
    // overlay lo anima. Solo loopback, asi que no hay riesgo de que lo use nadie
    // de afuera.
    // Banco de pruebas del punto 2: POST /banco {"punto":2,"camera":2,"activo":true}
    m_server->Post("/banco", [this](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Content-Type", "application/json");
        auto num = [&req](const char* clave, int porDefecto) {
            const std::string k = std::string("\"") + clave + "\"";
            const size_t p = req.body.find(k);
            if (p == std::string::npos) return porDefecto;
            const size_t d = req.body.find(':', p + k.size());
            if (d == std::string::npos) return porDefecto;
            return std::atoi(req.body.c_str() + d + 1);
        };
        const int punto = num("punto", 2);
        const int camara = num("camera", 2);
        const bool activo = req.body.find("false") == std::string::npos;
        BancoCallback cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cb = m_bancoCb;
        }
        if (!cb) {
            res.status = 400;
            res.set_content(R"({"ok":false,"motivo":"banco no cableado"})", "application/json");
            return;
        }
        cb(punto, camara, activo);
        res.set_content(std::string(R"({"ok":true,"activo":)") + (activo ? "true" : "false") + "}",
                        "application/json");
    });

    m_server->Post("/api/ranking", [this](const httplib::Request& req, httplib::Response& res) {
        if (req.body.empty()) {
            res.status = 400;
            res.set_content(R"({"ok":false,"motivo":"body vacio"})", "application/json");
            return;
        }
        Publish(req.body);
        res.set_content(R"({"ok":true})", "application/json");
    });

    // Estaticos: se sirve el directorio del overlay. httplib resuelve "/" a
    // index.html y no deja salir del directorio montado.
    const std::string dir = m_config.dir;
    if (!m_server->set_mount_point("/", dir.c_str())) {
        Logger::Warning("OverlayServer: no encontre el directorio '" + dir +
                        "'; el overlay no se va a servir");
    }

    const uint16_t port = m_config.port;
    m_thread = std::thread([this, port]() {
        const bool ok = m_server->listen("127.0.0.1", static_cast<int>(port));
        if (!ok) {
            Logger::Warning("OverlayServer: no pude escuchar en 127.0.0.1:" + std::to_string(port));
            m_running.store(false);
        }
    });

    // httplib no avisa cuando el bind fallo; se sondea el puerto un instante.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    httplib::Client probe("127.0.0.1", static_cast<int>(port));
    probe.set_connection_timeout(0, 300000);
    const auto r = probe.Get("/healthz");
    if (!r || r->status != 200) {
        Logger::Warning("OverlayServer: el servidor no respondio en 127.0.0.1:" + std::to_string(port));
        m_running.store(false);
        return false;
    }

    m_running.store(true);
    Logger::Info("OverlayServer: escuchando en http://127.0.0.1:" + std::to_string(port) +
                 " (overlay='" + dir + "')");
    return true;
}

void OverlayServer::Stop() {
    if (m_server) {
        m_server->stop();
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_running.exchange(false)) {
        Logger::Info("OverlayServer: detenido");
    }
}

void OverlayServer::SetBancoCallback(BancoCallback cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_bancoCb = std::move(cb);
}

void OverlayServer::Publish(const std::string& payload) {
    uint64_t version = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastPayload = payload;
        m_lastMs = NowEpochMs();
        version = ++m_version;
    }
    m_cv.notify_all();
    Logger::Info("Overlay: ranking v" + std::to_string(version) +
                 " -> " + std::to_string(m_clients.load()) + " cliente(s) -> " + payload);
    // Etapa 2: aca se empuja el payload a los clientes SSE de /events.
}
