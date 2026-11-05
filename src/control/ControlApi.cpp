/**
 * ControlApi.cpp
 *
 * Implementacion de la API HTTP de control (modo --service).
 *
 * Servidor: cpp-httplib (single header, MIT) en third_party/httplib.h.
 * Se eligio httplib y no un Winsock a mano porque ya resuelve el parseo de
 * request/response, el pool de conexiones y los timeouts, y el proyecto ya
 * linkea Ws2_32. La autenticacion (IP + X-Api-Key) va en el pre-routing handler:
 * asi no puede quedar un endpoint nuevo sin proteger.
 *
 * Nada de esto toca el hilo de captura: /arm encola una sonda en la misma cola
 * del hilo medidor (RankingMeasure::EnqueueProbe) y espera el resultado por el
 * callback de resultados.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// httplib primero: incluye winsock2.h y deja _WINSOCKAPI_ definido, asi que un
// windows.h posterior (via Logger/Engine) no arrastra winsock.h.
#include "../../third_party/httplib.h"

#include "ControlApi.h"

#include "../choreography/ChoreographyEngine.h"
#include "../control/PTZController.h"
#include "../control/TrackPhysicalController.h"
#include "../measure/RankingMeasure.h"
#include "../utils/Logger.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <thread>

namespace {

int64_t NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/**
 * Escapa un string para JSON (comillas, barra y controles). Los ids de carrera
 * son ASCII, pero el campo lo escribe el backend: no se confia.
 */
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

/**
 * Extrae el primer campo string de un JSON chico: {"race_id":"..."}.
 * No se trae un parser completo para esto: el body del /arm es de una sola
 * clave y cualquier otra cosa se rechaza por "falta race_id".
 */
std::string ExtractJsonString(const std::string& body, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) {
        return std::string();
    }
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return std::string();
    }
    pos = body.find('"', pos + 1);
    if (pos == std::string::npos) {
        return std::string();
    }
    std::string value;
    for (size_t i = pos + 1; i < body.size(); ++i) {
        const char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            value += body[++i];
            continue;
        }
        if (c == '"') {
            return value;
        }
        value += c;
    }
    return std::string();
}

/**
 * Allowlist de origen: loopback y la tailnet (100.64.0.0/10 = 100.64.0.0 ..
 * 100.127.255.255). Cualquier otra direccion se rechaza con 403: la API no se
 * expone a internet.
 */
bool IsAllowedRemote(const std::string& remoteAddr) {
    std::string ip = remoteAddr;
    if (ip.rfind("::ffff:", 0) == 0) {
        ip = ip.substr(7);   // IPv4 mapeada en IPv6
    }
    if (ip == "127.0.0.1" || ip == "::1") {
        return true;
    }
    if (ip.empty()) {
        return false;   // sin direccion no hay allowlist que valga
    }
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }
    if (a != 100) {
        return false;
    }
    return b >= 64 && b <= 127;   // prefijo /10
}

const char* EngineStateName(const Choreography::ChoreographyEngine* engine) {
    if (!engine) {
        return "disabled";
    }
    switch (engine->GetState()) {
        case Choreography::EngineState::Idle:     return "idle";
        case Choreography::EngineState::Ready:    return "ready";
        case Choreography::EngineState::Running:  return "running";
        case Choreography::EngineState::Paused:   return "paused";
        case Choreography::EngineState::Stopping: return "stopping";
        case Choreography::EngineState::Error:    return "error";
        default:                                  return "unknown";
    }
}

} // namespace

/**
 * httplib vive aca: el header de ControlApi no lo ve nadie mas.
 */
struct ControlApi::Impl {
    httplib::Server server;
    std::thread thread;
};

ControlApi::ControlApi() = default;

ControlApi::~ControlApi() {
    Stop();
}

const char* ControlApi::StateName(State state) {
    switch (state) {
        case State::Idle:     return "idle";
        case State::Armed:    return "armed";
        case State::Running:  return "running";
        case State::Finished: return "finished";
        default:              return "idle";
    }
}

ControlApi::State ControlApi::GetState() {
    std::lock_guard<std::mutex> lock(m_stateMutex);

    // El guion puede terminar solo (o fallar y quedar en Error): la API lo
    // refleja sin que nadie pregunte. Margen de 2 s sobre el /start para no
    // confundir "todavia no arranco el hilo" con "termino".
    if (m_state == State::Running) {
        if (m_engine) {
            const int64_t now = NowEpochMs();
            const auto engineState = m_engine->GetState();
            const bool stopped = (engineState == Choreography::EngineState::Ready ||
                                  engineState == Choreography::EngineState::Idle ||
                                  engineState == Choreography::EngineState::Error);
            if (m_engine->IsFinished() || (stopped && now - m_startedAt > 2000)) {
                Logger::Info(std::string("[API] coreografia terminada (estado del motor: ") +
                             EngineStateName(m_engine) + "), state=finished");
                m_state = State::Finished;
            }
        } else {
            m_state = State::Finished;
        }
    }
    return m_state;
}

/**
 * Foto para el panel de consola del menu [12] SERVICIO API.
 *
 * Se arma con las mismas fuentes que GET /status (estado del motor, ultimo
 * ranking publicado) mas el ultimo comando registrado por el logger. Cada
 * campo se toma bajo su propio lock: no se retiene m_stateMutex mientras se
 * consulta al medidor.
 */
ControlApi::Snapshot ControlApi::GetSnapshot() {
    Snapshot snapshot;

    snapshot.listening = m_listening.load();
    snapshot.armInFlight = m_armInFlight.load();
    snapshot.state = GetState();          // puede pasar running -> finished
    snapshot.stateName = StateName(snapshot.state);
    snapshot.engine = EngineStateName(m_engine);

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        snapshot.raceId = m_raceId;
        snapshot.t0 = m_t0;
    }

    if (m_measure) {
        std::vector<int> ranking;
        int point = 0;
        int64_t rankingEpochMs = 0;
        m_measure->GetLastRanking(ranking, point, rankingEpochMs);
        snapshot.point = point;
        snapshot.spheres = static_cast<int>(ranking.size());
    }

    {
        std::lock_guard<std::mutex> lock(m_lastCmdMutex);
        snapshot.lastCommand = m_lastCmdPath;
        snapshot.lastStatus = m_lastCmdStatus;
        snapshot.lastAtMs = m_lastCmdAt;
    }

    return snapshot;
}

void ControlApi::RecordCommand(const std::string& path, int httpStatus) {
    // "/arm" -> "arm": la barra no aporta nada en una linea de consola.
    const std::string name = (!path.empty() && path.front() == '/')
                                 ? path.substr(1)
                                 : path;
    std::lock_guard<std::mutex> lock(m_lastCmdMutex);
    m_lastCmdPath = name;
    m_lastCmdStatus = httpStatus;
    m_lastCmdAt = NowEpochMs();
}

bool ControlApi::ShutdownRequested() const {
    if (!m_shutdownRequested.load()) {
        return false;
    }
    const int64_t at = m_shutdownAt.load();
    return at == 0 || NowEpochMs() >= at;
}

bool ControlApi::Start(const ControlApiConfig& config,
                       Choreography::ChoreographyEngine* engine,
                       Measure::RankingMeasure* measure,
                       const std::string& scriptPath,
                       TrackPhysicalController* track,
                       PTZ::PTZController* ptz) {
    if (m_listening.load()) {
        return true;
    }
    if (!config.enabled) {
        Logger::Info("[API] control_api.enabled=false: no se levanta el servidor");
        return false;
    }
    if (config.apiKey.empty()) {
        Logger::Error("[API] control_api.api_key vacia: la API NO se levanta "
                      "(seria un control sin autenticacion)");
        return false;
    }
    if (config.port <= 0 || config.port > 65535) {
        Logger::Error("[API] control_api.port invalido: " + std::to_string(config.port));
        return false;
    }

    m_config = config;
    m_engine = engine;
    m_measure = measure;
    m_track = track;
    m_ptz = ptz;
    m_scriptPath = scriptPath;
    m_impl = std::make_unique<Impl>();

    if (config.apiKey == "CAMBIAR") {
        Logger::Warning("[API] control_api.api_key sigue en el valor de ejemplo "
                        "\"CAMBIAR\": cambiar en config.json antes de produccion");
    }

    auto& server = m_impl->server;

    // /arm es el armado completo: lock META (dos camaras) + reload + espera del
    // ascensor + lock ARRIBA (dos camaras). El timeout tiene que cubrir CUATRO
    // rafagas, sus asentamientos de PTZ y la espera (holgado: 240 s).
    server.set_read_timeout(15, 0);
    server.set_write_timeout(240, 0);
    server.set_idle_interval(0, 200000);
    server.set_payload_max_length(64 * 1024);

    server.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
        Logger::Debug("[API] " + req.method + " " + req.path + " <- " + req.remote_addr +
                      " -> " + std::to_string(res.status));
        // Panel del menu [12]: se recuerda el ultimo comando que MUEVE el
        // sistema. /status queda afuera a proposito: lo consulta el backend
        // cada segundo y taparia al /arm, que es lo que el operador mira.
        if (req.path != "/status") {
            RecordCommand(req.path, res.status);
        }
    });

    // Autenticacion ANTES de rutear: ningun endpoint queda sin key ni allowlist.
    server.set_pre_routing_handler([this](const httplib::Request& req,
                                          httplib::Response& res) {
        const std::string& remote = req.remote_addr;
        if (!IsAllowedRemote(remote)) {
            Logger::Warning("[API] rechazado por IP: " + remote + " (" + req.method +
                            " " + req.path + ")");
            res.status = 403;
            res.set_content("{\"ok\":false,\"motivo\":\"IP no permitida\"}",
                            "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }

        // Se busca en los headers del request
        const std::string key = req.get_header_value("X-Api-Key");
        if (key != m_config.apiKey) {
            Logger::Warning("[API] rechazado por api key: " + remote + " (" + req.method +
                            " " + req.path + ")");
            res.status = 401;
            res.set_content("{\"ok\":false,\"motivo\":\"api key invalida\"}",
                            "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server.Get("/status", [this](const httplib::Request&, httplib::Response& res) {
        int status = 500;
        std::string body;
        HandleStatus(status, body);
        res.status = status;
        res.set_content(body, "application/json");
    });

    server.Post("/arm", [this](const httplib::Request& req, httplib::Response& res) {
        int status = 500;
        std::string body;
        HandleArm(req.body, status, body);
        res.status = status;
        res.set_content(body, "application/json");
    });

    server.Post("/start", [this](const httplib::Request&, httplib::Response& res) {
        int status = 500;
        std::string body;
        HandleStart(status, body);
        res.status = status;
        res.set_content(body, "application/json");
    });

    server.Post("/abort", [this](const httplib::Request&, httplib::Response& res) {
        int status = 500;
        std::string body;
        HandleAbort(status, body);
        res.status = status;
        res.set_content(body, "application/json");
    });

    server.Post("/shutdown", [this](const httplib::Request&, httplib::Response& res) {
        int status = 500;
        std::string body;
        HandleShutdown(status, body);
        res.status = status;
        res.set_content(body, "application/json");
    });

    server.set_exception_handler([](const httplib::Request& req, httplib::Response& res,
                                    std::exception_ptr ep) {
        std::string what = "desconocido";
        try {
            if (ep) {
                std::rethrow_exception(ep);
            }
        } catch (const std::exception& e) {
            what = e.what();
        } catch (...) {
        }
        Logger::Error("[API] excepcion en " + req.method + " " + req.path + ": " + what);
        res.status = 500;
        res.set_content("{\"ok\":false,\"motivo\":\"error interno\"}", "application/json");
    });

    // bind_to_port primero: si el puerto esta tomado se sabe ACA, no dentro del hilo.
    if (!server.bind_to_port("0.0.0.0", m_config.port)) {
        Logger::Error("[API] no se pudo escuchar en 0.0.0.0:" + std::to_string(m_config.port) +
                      " (puerto ocupado?)");
        m_impl.reset();
        return false;
    }

    m_impl->thread = std::thread([this]() {
        if (!m_impl->server.listen_after_bind()) {
            Logger::Error("[API] listen_after_bind() fallo");
            m_listening = false;
        }
    });

    // Esperar a que el server este arriba: sin esto el backend puede llegar
    // antes de que el puerto acepte y ver un connection refused en el arranque.
    const int64_t deadline = NowEpochMs() + 3000;
    while (!server.is_running() && NowEpochMs() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!server.is_running()) {
        Logger::Error("[API] el servidor no arranco en 3 s");
        server.stop();
        if (m_impl->thread.joinable()) {
            m_impl->thread.join();
        }
        m_impl.reset();
        return false;
    }

    m_listening = true;

    // Resultados de las sondas de /arm: los entrega el hilo medidor. Se registra
    // recien cuando el server YA escucha: si el bind falla, el medidor no puede
    // quedar con un callback que apunte a este objeto.
    if (m_measure) {
        m_measure->SetResultCallback([this](const Measure::RankingMeasure::MeasureResult& result) {
            if (!result.probe) {
                return;   // /arm solo mira sondas; las mediciones de carrera no
            }
            OnProbeResult(result.jobId, result.spheres, result.motivo);
        });
    }

    Logger::Info("[API] escuchando en 0.0.0.0:" + std::to_string(m_config.port) +
                 " (allowlist: 127.0.0.1 + 100.64.0.0/10, header X-Api-Key obligatorio)");
    Logger::Info(std::string("[API] coreografia: ") + (m_engine ? "habilitada" : "DESHABILITADA") +
                 ", medidor: " + (m_measure ? "habilitado" : "DESHABILITADO") +
                 ", esp32: " + (m_track ? "conectado (reload disponible)" : "SIN controlador (no se puede recargar)") +
                 ", ptz: " + (m_ptz ? "habilitado" : "SIN controlador (se mide la vista actual)") +
                 ", guion=" + (m_scriptPath.empty() ? std::string("(sin script_path)") : m_scriptPath));
    Logger::Info("[API] armado automatico (dos camaras por lock, AND): META cam=" +
                 std::to_string(m_config.armMetaCamera) + "/" +
                 std::to_string(m_config.armMetaCamera2) +
                 " presets=" + std::to_string(m_config.armMetaPreset) + "/" +
                 std::to_string(m_config.armMetaPreset2) +
                 " -> reload -> espera " + std::to_string(m_config.armReloadWaitMs) + " ms" +
                 " -> ARRIBA cam=" +
                 (m_config.armArribaCamera > 0 ? std::to_string(m_config.armArribaCamera)
                                               : std::string("guion")) +
                 "/" + std::to_string(m_config.armArribaCamera2) +
                 " presets=" + std::to_string(m_config.armArribaPreset) + "/" +
                 std::to_string(m_config.armArribaPreset2));
    return true;
}

void ControlApi::Stop() {
    // El medidor no puede quedar con un callback que apunte a este objeto, ni
    // aunque el server nunca haya llegado a escuchar.
    if (m_measure) {
        m_measure->SetResultCallback(nullptr);
    }
    if (!m_impl) {
        m_listening = false;
        return;
    }
    Logger::Info("[API] bajando el servidor de control...");
    m_impl->server.stop();
    if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
    m_impl.reset();
    m_listening = false;
    Logger::Info("[API] servidor de control detenido");
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

void ControlApi::HandleStatus(int& status, std::string& out) {
    const State state = GetState();

    std::vector<int> ranking;
    int point = 0;
    int64_t rankingEpochMs = 0;
    if (m_measure) {
        m_measure->GetLastRanking(ranking, point, rankingEpochMs);
    }

    std::string raceId;
    int64_t t0 = 0;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        raceId = m_raceId;
        t0 = m_t0;
    }

    std::ostringstream oss;
    oss << "{\"state\":\"" << StateName(state) << "\""
        << ",\"race_id\":\"" << JsonEscape(raceId) << "\""
        << ",\"point\":" << point
        << ",\"spheres\":" << ranking.size()
        << ",\"ranking\":[";
    for (size_t i = 0; i < ranking.size(); ++i) {
        if (i > 0) {
            oss << ",";
        }
        oss << ranking[i];
    }
    oss << "]"
        << ",\"t0\":" << t0
        << ",\"engine\":\"" << EngineStateName(m_engine) << "\""
        << "}";

    status = 200;
    out = oss.str();
}

void ControlApi::HandleArm(const std::string& requestBody, int& status, std::string& out) {
    const std::string raceId = ExtractJsonString(requestBody, "race_id");
    if (raceId.empty()) {
        status = 400;
        out = "{\"ok\":false,\"spheres\":0,\"motivo\":\"falta race_id en el body\"}";
        return;
    }

    // GetState() refresca running->finished: si la carrera anterior termino sola
    // se puede armar la siguiente sin reiniciar el VIB.
    if (GetState() == State::Running) {
        status = 409;
        out = "{\"ok\":false,\"spheres\":0,\"motivo\":\"carrera en curso\"}";
        return;
    }

    if (!m_engine) {
        status = 409;
        out = "{\"ok\":false,\"spheres\":0,\"motivo\":\"coreografia deshabilitada en config\"}";
        return;
    }
    if (!m_measure) {
        status = 409;
        out = "{\"ok\":false,\"spheres\":0,"
              "\"motivo\":\"medidor no inicializado (ranking_publisher.enabled=false)\"}";
        return;
    }

    // Guion listo: si el motor no tiene uno cargado se carga aca (mismo archivo
    // que usaria F12). Sin guion no hay carrera que armar.
    if (!m_engine->HasScript()) {
        if (m_scriptPath.empty() || !m_engine->Load(m_scriptPath)) {
            status = 409;
            out = "{\"ok\":false,\"spheres\":0,\"motivo\":\"no se pudo cargar el guion: " +
                  JsonEscape(m_engine->GetLastError()) + "\"}";
            return;
        }
    }

    // Un solo armado a la vez: el /arm completo dura ~25 s (cuatro rafagas con
    // sus asentamientos + la espera del ascensor). El segundo pedido recibe
    // 409, no otra sonda.
    if (m_armInFlight.exchange(true)) {
        status = 409;
        out = "{\"ok\":false,\"spheres\":0,\"etapa\":\"arm\","
              "\"motivo\":\"ya hay un /arm en curso\"}";
        return;
    }
    struct InFlightGuard {
        std::atomic<bool>* flag;
        ~InFlightGuard() { flag->store(false); }
    } inFlightGuard{&m_armInFlight};
    m_armCancelled = false;

    // Mientras se arma no hay nada armado: si una etapa falla, /start tiene que
    // responder "no esta armado". Se pasa a armed recien al final, con los dos
    // locks verificados en 10 por sus dos camaras.
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state = State::Idle;
        m_t0 = 0;
    }

    // Camara primaria y ventana de ARRIBA: manda el guion (es lo que va a medir
    // la carrera en el punto 1), para verificar exactamente lo que va a medir
    // la carrera. arm_arriba_camera NO pisa al guion: solo se usa cuando el
    // guion no tiene ese punto (si no, el /arm estaria verificando una camara
    // distinta de la que mide la carrera, que es el 10 falso que se evita).
    // La segunda camara del lock (arm_arriba_camera2) es un testigo extra.
    int arribaCamera = (m_config.armArribaCamera > 0) ? m_config.armArribaCamera
                                                     : m_config.armCamera;
    int arribaBurst = m_config.armBurstMs;
    int arribaMax = m_config.armMaxMs;
    int scriptCamera = 0, scriptBurst = 0, scriptMax = 0;
    if (m_engine->GetMeasurePointFor(1, scriptCamera, scriptBurst, scriptMax)) {
        if (m_config.armArribaCamera > 0 && m_config.armArribaCamera != scriptCamera) {
            Logger::Warning("[API] /arm ARRIBA: arm_arriba_camera=" +
                            std::to_string(m_config.armArribaCamera) + " se ignora porque el guion "
                            "tiene el punto 1 en la camara " + std::to_string(scriptCamera) +
                            ": manda el guion (la carrera mide esa camara)");
        }
        arribaCamera = scriptCamera;
        arribaBurst = scriptBurst;
        arribaMax = scriptMax;
        Logger::Info("[API] /arm ARRIBA con la camara y la ventana del guion (punto 1): cam=" +
                     std::to_string(arribaCamera) + " burst=" + std::to_string(arribaBurst) +
                     "ms max=" + std::to_string(arribaMax) + "ms");
    } else {
        Logger::Warning("[API] /arm: el guion no tiene MeasurePoint para el punto 1; "
                        "ARRIBA se mide con control_api (" + std::to_string(arribaCamera) + ", " +
                        std::to_string(arribaBurst) + "/" + std::to_string(arribaMax) + "ms)");
    }

    // ------------------------------------------------------------------
    // Configuracion de los DOS locks, ANTES de mover nada.
    // Cada lock exige DOS camaras (AND). Ninguna se adivina: sin declarar, se
    // corta aca con un motivo que dice que clave configurar. Medir otra camara
    // daria un "10" falso y el armado dejaria de significar algo.
    // Se valida ARRIBA tambien ahora: si faltara su segunda camara, no tiene
    // sentido mandar el reload y dejar las 10 esferas arriba para despues
    // rechazar el armado.
    // ------------------------------------------------------------------
    if (m_config.armMetaCamera <= 0) {
        ArmFail(status, out, "meta", 0,
                "control_api.arm_meta_camera no configurada: no se sabe con que camara "
                "se ve la META (donde estan las esferas antes del reload)");
        return;
    }
    const int metaCamera = m_config.armMetaCamera;
    const int metaCamera2 = m_config.armMetaCamera2;
    if (metaCamera2 <= 0) {
        ArmFail(status, out, "meta", 0,
                "control_api.arm_meta_camera2 no configurada: el lock META exige DOS camaras "
                "(AND) y con una sola no hay verificacion cruzada");
        return;
    }
    if (metaCamera2 == metaCamera) {
        ArmFail(status, out, "meta", 0,
                "las dos camaras del lock META son la misma (" + std::to_string(metaCamera) +
                "): el AND no verificaria nada; declarar otra camara en arm_meta_camera2");
        return;
    }
    if (arribaCamera <= 0) {
        ArmFail(status, out, "arriba", 0,
                "no se sabe con que camara se ve ARRIBA: el guion no tiene MeasurePoint para "
                "el punto 1 y control_api.arm_camera/arm_arriba_camera tampoco");
        return;
    }
    const int arribaCamera2 = m_config.armArribaCamera2;
    if (arribaCamera2 <= 0) {
        ArmFail(status, out, "arriba", 0,
                "control_api.arm_arriba_camera2 no configurada: el lock ARRIBA exige DOS camaras "
                "(AND) y con una sola no hay verificacion cruzada");
        return;
    }
    if (arribaCamera2 == arribaCamera) {
        ArmFail(status, out, "arriba", 0,
                "las dos camaras del lock ARRIBA son la misma (" + std::to_string(arribaCamera) +
                "): el AND no verificaria nada; declarar otra camara en arm_arriba_camera2");
        return;
    }

    // Rango de las cuatro camaras, tambien antes de mover nada: una camara
    // inexistente rechazaria la sonda recien en la etapa ARRIBA, o sea DESPUES
    // del reload, con las 10 esferas ya arriba y el armado en idle.
    auto fueraDeRango = [&](int camera, const char* etapa, const char* clave) {
        const int maxCamera = Measure::RankingMeasure::kMaxCameras;
        if (camera >= 1 && camera <= maxCamera) {
            return false;
        }
        ArmFail(status, out, etapa, 0,
                std::string(clave) + " = " + std::to_string(camera) +
                ": fuera de rango (el medidor tiene " + std::to_string(maxCamera) +
                " camaras, 1.." + std::to_string(maxCamera) + ")");
        return true;
    };
    if (fueraDeRango(metaCamera, "meta", "control_api.arm_meta_camera") ||
        fueraDeRango(metaCamera2, "meta", "control_api.arm_meta_camera2") ||
        fueraDeRango(arribaCamera, "arriba", "la camara de ARRIBA (punto 1 del guion) / arm_camera") ||
        fueraDeRango(arribaCamera2, "arriba", "control_api.arm_arriba_camera2")) {
        return;
    }

    const int64_t armStartMs = NowEpochMs();
    std::string motivo;

    // ------------------------------------------------------------------
    // ETAPA 1: lock META (dos camaras, AND). Sin 10 en las dos no se manda el
    // reload.
    // ------------------------------------------------------------------
    const std::vector<ArmLock::CameraSpec> metaCams = {
        {metaCamera, m_config.armMetaPreset},
        {metaCamera2, m_config.armMetaPreset2},
    };
    ArmLock::Verdict metaVerdict;
    RunArmLock("META", metaCams, m_config.armMetaBurstMs, m_config.armMetaMaxMs, metaVerdict);
    if (!metaVerdict.ok) {
        ArmFail(status, out, "meta", metaVerdict.failingSpheres, metaVerdict.motivo,
                metaVerdict.failingCamera, ArmCamerasJson(metaVerdict.readings));
        return;
    }
    // Conteo de la camara primaria: "spheres" del 200 y de las etapas que no
    // miden (reload/espera) sigue siendo el mismo campo de siempre.
    const int metaSpheres = metaVerdict.readings.front().spheres;
    Logger::Info("[API] /arm META OK (AND de dos camaras): " + metaVerdict.detalle);

    // ------------------------------------------------------------------
    // ETAPA 2: reload del ESP32 (abre el ascensor: suben a la salida).
    // ------------------------------------------------------------------
    if (!m_track) {
        ArmFail(status, out, "reload", metaSpheres,
                "sin controlador del ESP32: no se puede mandar el reload");
        return;
    }
    if (m_armCancelled.load()) {
        ArmFail(status, out, "reload", metaSpheres, "armado cancelado por /abort");
        return;
    }
    if (!m_track->abrirAscensor()) {
        ArmFail(status, out, "reload", metaSpheres,
                "el ESP32 no acepto /recargar (reload); revisar la red del ESP32 y su log");
        return;
    }
    Logger::Info("[API] /arm: reload enviado al ESP32; esperando " +
                 std::to_string(m_config.armReloadWaitMs) + " ms a que suban las esferas");

    // ------------------------------------------------------------------
    // ETAPA 3: espera del ascensor. El guion la tiene medida: reload en
    // t=9,0 s y medicion del punto 1 en t=15,0 s -> 6 s.
    // ------------------------------------------------------------------
    if (!WaitArmDelay(m_config.armReloadWaitMs, motivo)) {
        ArmFail(status, out, "espera", metaSpheres, motivo);
        return;
    }

    // ------------------------------------------------------------------
    // ETAPA 4: lock ARRIBA (salida), dos camaras y AND. Tienen que haber
    // llegado las 10 a las dos vistas.
    // ------------------------------------------------------------------
    const std::vector<ArmLock::CameraSpec> arribaCams = {
        {arribaCamera, m_config.armArribaPreset},
        {arribaCamera2, m_config.armArribaPreset2},
    };
    ArmLock::Verdict arribaVerdict;
    RunArmLock("ARRIBA", arribaCams, arribaBurst, arribaMax, arribaVerdict);
    if (!arribaVerdict.ok) {
        ArmFail(status, out, "arriba", arribaVerdict.failingSpheres, arribaVerdict.motivo,
                arribaVerdict.failingCamera, ArmCamerasJson(arribaVerdict.readings));
        return;
    }
    const int arribaSpheres = arribaVerdict.readings.front().spheres;
    Logger::Info("[API] /arm ARRIBA OK (AND de dos camaras): " + arribaVerdict.detalle);

    // Armado completo: recien aca el estado pasa a armed.
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state = State::Armed;
        m_raceId = raceId;
        m_t0 = 0;
    }
    // Identidades limpias para la carrera: las cuatro sondas (dos por lock)
    // dejaron tracks propios.
    m_measure->ResetIdentity();

    const int64_t totalMs = NowEpochMs() - armStartMs;
    std::ostringstream oss;
    oss << "{\"ok\":true,\"spheres\":" << arribaSpheres
        << ",\"race_id\":\"" << JsonEscape(raceId) << "\""
        << ",\"meta\":{\"camera\":" << metaCamera
        << ",\"preset\":" << m_config.armMetaPreset
        << ",\"spheres\":" << metaSpheres
        << ",\"cameras\":" << ArmCamerasJson(metaVerdict.readings) << "}"
        << ",\"reload\":{\"ok\":true,\"wait_ms\":" << m_config.armReloadWaitMs << "}"
        << ",\"arriba\":{\"camera\":" << arribaCamera
        << ",\"preset\":" << m_config.armArribaPreset
        << ",\"spheres\":" << arribaSpheres
        << ",\"cameras\":" << ArmCamerasJson(arribaVerdict.readings) << "}"
        << ",\"camera\":" << arribaCamera
        << ",\"total_ms\":" << totalMs
        << "}";
    status = 200;
    out = oss.str();
    Logger::Info("[API] /arm OK race_id=" + raceId + " (AND de dos camaras en cada lock): " +
                 "META " + metaVerdict.detalle + " -> reload -> " +
                 std::to_string(m_config.armReloadWaitMs) + " ms -> ARRIBA " +
                 arribaVerdict.detalle + " en " + std::to_string(totalMs) +
                 " ms: state=armed");
}

void ControlApi::HandleStart(int& status, std::string& out) {
    const State current = GetState();
    if (current != State::Armed) {
        status = 409;
        out = std::string("{\"ok\":false,\"motivo\":\"no esta armado\",\"state\":\"") +
              StateName(current) + "\"}";
        return;
    }
    if (!m_engine) {
        status = 409;
        out = "{\"ok\":false,\"motivo\":\"coreografia deshabilitada en config\"}";
        return;
    }

    // Equivale al F12: arranca (o reanuda) el guion desde el principio.
    bool started = false;
    const auto engineState = m_engine->GetState();
    if (engineState == Choreography::EngineState::Error) {
        Logger::Info("[API] /start: el motor estaba en Error, se recarga el guion");
        if (!m_scriptPath.empty() && m_engine->Load(m_scriptPath)) {
            started = m_engine->Start();
        }
    } else if (engineState == Choreography::EngineState::Paused) {
        m_engine->Resume();
        started = true;
    } else {
        started = m_engine->Start();
    }

    if (!started) {
        status = 500;
        out = "{\"ok\":false,\"motivo\":\"" + JsonEscape(m_engine->GetLastError()) + "\"}";
        Logger::Error("[API] /start fallo: " + m_engine->GetLastError());
        return;
    }

    const int64_t t0 = NowEpochMs();
    std::string raceId;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state = State::Running;
        m_t0 = t0;
        m_startedAt = t0;
        raceId = m_raceId;
    }
    // Ranking de la carrera anterior fuera: /status no debe mostrar el final
    // viejo mientras esta carrera todavia no publico su primer punto.
    if (m_measure) {
        m_measure->ClearLastRanking();
    }

    status = 200;
    out = "{\"ok\":true,\"t0\":" + std::to_string(t0) +
          ",\"race_id\":\"" + JsonEscape(raceId) + "\"}";
    Logger::Info("[API] /start OK race_id=" + raceId + " t0=" + std::to_string(t0) +
                 ": state=running");
}

void ControlApi::HandleAbort(int& status, std::string& out) {
    // Si hay un /arm en curso, que no siga: la espera del ascensor se corta y
    // las etapas que faltan no se ejecutan.
    m_armCancelled = true;
    if (m_engine) {
        Logger::Info("[API] /abort: deteniendo la coreografia");
        m_engine->Stop();
    }
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state = State::Idle;
        m_t0 = 0;
    }
    status = 200;
    out = "{\"ok\":true,\"state\":\"idle\"}";
    Logger::Info("[API] /abort OK: state=idle");
}

void ControlApi::HandleShutdown(int& status, std::string& out) {
    // En --service no hay teclado: esta es la unica forma ordenada de apagar.
    // Se demora el pedido 500 ms para que la respuesta salga antes de que el
    // bucle principal empiece a desarmar el pipeline.
    m_shutdownAt = NowEpochMs() + 500;
    m_shutdownRequested = true;
    status = 200;
    out = "{\"ok\":true,\"bye\":true}";
    Logger::Info("[API] /shutdown: apagando el VIB");
}

/**
 * Corre UNA medicion de sonda en la camara pedida y espera el resultado.
 * Encaja en el medidor existente: EnqueueProbe usa la misma cola del hilo
 * medidor (no abre clip, no publica por UDP), asi que no bloquea la captura.
 */
bool ControlApi::DoArmProbe(int camera1Based, int burstMs, int maxMs,
                            int& spheres, std::string& motivo) {
    spheres = 0;
    motivo.clear();

    if (!m_measure) {
        motivo = "medidor no inicializado";
        return false;
    }
    // El "un /arm a la vez" lo lleva HandleArm: aca pueden entrar dos sondas
    // seguidas (META y ARRIBA) del MISMO armado, que es lo correcto.

    const int camera = (camera1Based > 0) ? (camera1Based - 1) : 0;   // guion 1-indexed
    const int burst = (burstMs > 0) ? burstMs : 400;
    const int hardMax = (maxMs > burst) ? maxMs : burst;

    const int64_t jobId = m_measure->EnqueueProbe(camera, burst, hardMax);
    if (jobId == 0) {
        motivo = "el medidor rechazo la sonda (camara fuera de rango o medidor no inicializado)";
        return false;
    }

    // Margen: la rafaga (burst + max) mas el volcado del clip de esa camara.
    const int64_t timeoutMs = static_cast<int64_t>(hardMax) + 15000;
    std::unique_lock<std::mutex> lock(m_resultMutex);
    const bool done = m_resultCv.wait_for(
        lock, std::chrono::milliseconds(timeoutMs),
        [this, jobId]() { return m_probeDone && m_probeJobId == jobId; });

    if (!done) {
        motivo = "sin resultado de la sonda en " + std::to_string(timeoutMs) + " ms";
        Logger::Warning("[API] /arm: " + motivo);
        return false;
    }

    spheres = m_probeSpheres;
    motivo = m_probeMotivo;
    m_probeDone = false;
    m_probeJobId = 0;
    return true;
}

void ControlApi::OnProbeResult(int64_t jobId, int spheres, const std::string& motivo) {
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        m_probeJobId = jobId;
        m_probeSpheres = spheres;
        m_probeMotivo = motivo;
        m_probeDone = true;
    }
    m_resultCv.notify_all();
}


// ---------------------------------------------------------------------------
// Armado automatico: etapas que no son medicion
// ---------------------------------------------------------------------------

std::string ControlApi::ArmCamerasJson(const std::vector<ArmLock::CameraReading>& readings) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < readings.size(); ++i) {
        const ArmLock::CameraReading& reading = readings[i];
        if (i > 0) {
            oss << ",";
        }
        oss << "{\"camera\":" << reading.camera
            << ",\"preset\":" << reading.preset
            << ",\"spheres\":" << reading.spheres
            // "ok" de ESA camara: midio y dio el target. En el 409 queda claro
            // cual fallo sin tener que restar.
            << ",\"ok\":"
            << ((reading.measured && reading.spheres == Measure::RankingMeasure::kTargetSpheres)
                    ? "true" : "false")
            << "}";
    }
    oss << "]";
    return oss.str();
}

void ControlApi::RunArmLock(const char* etapa,
                            const std::vector<ArmLock::CameraSpec>& cameras,
                            int burstMs, int maxMs,
                            ArmLock::Verdict& verdict) {
    // La secuencia (encuadrar TODAS -> medir TODAS -> AND) y su veredicto viven
    // en ArmLock.h, que se prueba en tests/test_arm_lock.cpp. Aca solo se
    // conectan los tres verbos reales: PTZ, medidor y cancelacion.
    auto move = [this, etapa](int camera, int preset, std::string& motivo) {
        if (m_armCancelled.load() || m_shutdownRequested.load()) {
            motivo = "armado cancelado (/abort o /shutdown) antes de encuadrar la camara " +
                     std::to_string(camera);
            return false;
        }
        return MoveArmCamera(camera, preset, etapa, motivo);
    };
    auto probe = [this, burstMs, maxMs](int camera, int /*preset*/, int& spheres,
                                        std::string& motivo) {
        if (m_armCancelled.load() || m_shutdownRequested.load()) {
            motivo = "armado cancelado (/abort o /shutdown) antes de medir la camara " +
                     std::to_string(camera);
            return false;
        }
        return DoArmProbe(camera, burstMs, maxMs, spheres, motivo);
    };

    verdict = ArmLock::Run(cameras, Measure::RankingMeasure::kTargetSpheres, move, probe);

    if (!verdict.ok) {
        Logger::Warning(std::string("[API] /arm lock ") + etapa + " rechazado (cam " +
                        std::to_string(verdict.failingCamera) + "): " + verdict.motivo);
        return;
    }
    Logger::Info(std::string("[API] /arm lock ") + etapa + " OK: " + verdict.detalle);
}

bool ControlApi::MoveArmCamera(int camera1Based, int preset, const char* etapa,
                               std::string& motivo) {
    motivo.clear();
    if (preset <= 0) {
        Logger::Info(std::string("[API] /arm ") + etapa + ": camara " +
                     std::to_string(camera1Based) +
                     " sin preset configurado, se mide la vista actual");
        return true;
    }
    if (!m_ptz) {
        Logger::Warning(std::string("[API] /arm ") + etapa + ": camara " +
                        std::to_string(camera1Based) + " con preset " +
                        std::to_string(preset) + " configurado pero sin PTZController; "
                        "se mide la vista actual (el encuadre NO se verifica)");
        return true;
    }
    if (camera1Based <= 0) {
        motivo = std::string("camara invalida para el preset de ") + etapa;
        return false;
    }

    // El guion y el config numera las camaras 1-indexed; PTZController, 0-indexed.
    const int cameraIndex = camera1Based - 1;
    Logger::Info(std::string("[API] /arm ") + etapa + ": PTZ camara " +
                 std::to_string(camera1Based) + " -> preset " + std::to_string(preset) +
                 " (asentando " + std::to_string(m_config.armPtzSettleMs) + " ms)");
    const auto viscaResult = m_ptz->CallPreset(cameraIndex, preset);
    if (!viscaResult.success) {
        motivo = std::string("el PTZ rechazo el preset ") + std::to_string(preset) +
                 " de la camara " + std::to_string(camera1Based) + " para ver " + etapa +
                 ": " + viscaResult.errorMessage;
        return false;
    }

    // El motor tarda en llegar y enfocar; medir en movimiento da cajas borrosas.
    // Una cancelacion durante el asentamiento la ve la etapa siguiente.
    std::string ignored;
    WaitArmDelay(m_config.armPtzSettleMs, ignored);
    return true;
}

bool ControlApi::WaitArmDelay(int ms, std::string& motivo) {
    motivo.clear();
    if (ms <= 0) {
        return true;
    }
    const int64_t deadline = NowEpochMs() + ms;
    while (NowEpochMs() < deadline) {
        if (m_armCancelled.load() || m_shutdownRequested.load()) {
            motivo = "armado cancelado durante la espera (/abort o /shutdown)";
            return false;
        }
        const int64_t remaining = deadline - NowEpochMs();
        const int64_t slice = (remaining < 100) ? ((remaining > 0) ? remaining : 1) : 100;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
    }
    return true;
}

void ControlApi::ArmFail(int& status, std::string& out, const char* etapa,
                         int spheres, const std::string& motivo,
                         int camera, const std::string& camerasJson) {
    // Un armado a medias no se puede disparar: state=idle y /start responde
    // "no esta armado" hasta que un /arm completo pase las etapas.
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state = State::Idle;
        m_t0 = 0;
    }
    status = 409;
    out = std::string("{\"ok\":false,\"spheres\":") + std::to_string(spheres) +
          ",\"etapa\":\"" + etapa + "\"";
    // El 409 de un lock nombra la camara que fallo y trae el conteo de las DOS:
    // con un solo numero el backend no puede distinguir "una camara ciega" de
    // "las dos a medias".
    if (camera > 0) {
        out += ",\"camera\":" + std::to_string(camera);
    }
    if (!camerasJson.empty()) {
        out += ",\"camaras\":" + camerasJson;
    }
    out += ",\"motivo\":\"" + JsonEscape(motivo) + "\"}";
    Logger::Warning(std::string("[API] /arm rechazado en la etapa ") + etapa +
                    (camera > 0 ? (" (cam " + std::to_string(camera) + ")") : std::string()) +
                    ": " + motivo);
}
