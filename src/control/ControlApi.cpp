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
                       const std::string& scriptPath) {
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
    m_scriptPath = scriptPath;
    m_impl = std::make_unique<Impl>();

    if (config.apiKey == "CAMBIAR") {
        Logger::Warning("[API] control_api.api_key sigue en el valor de ejemplo "
                        "\"CAMBIAR\": cambiar en config.json antes de produccion");
    }

    auto& server = m_impl->server;

    // /arm espera la rafaga completa del punto 1 (burst + max): el write timeout
    // tiene que cubrir esa espera mas el volcado del clip.
    server.set_read_timeout(15, 0);
    server.set_write_timeout(90, 0);
    server.set_idle_interval(0, 200000);
    server.set_payload_max_length(64 * 1024);

    server.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        Logger::Debug("[API] " + req.method + " " + req.path + " <- " + req.remote_addr +
                      " -> " + std::to_string(res.status));
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
                 ", guion=" + (m_scriptPath.empty() ? std::string("(sin script_path)") : m_scriptPath));
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

    // Camara y ventana del chequeo: manda el guion (es lo que va a medir la
    // carrera en el punto 1). Si el guion no tiene ese punto, el fallback de
    // control_api.
    int camera = m_config.armCamera;
    int burstMs = m_config.armBurstMs;
    int maxMs = m_config.armMaxMs;
    int scriptCamera = 0, scriptBurst = 0, scriptMax = 0;
    if (m_engine->GetMeasurePointFor(1, scriptCamera, scriptBurst, scriptMax)) {
        camera = scriptCamera;
        burstMs = scriptBurst;
        maxMs = scriptMax;
        Logger::Info("[API] /arm verifica el punto 1 con la camara y la ventana del guion: cam=" +
                     std::to_string(camera) + " burst=" + std::to_string(burstMs) +
                     "ms max=" + std::to_string(maxMs) + "ms");
    } else {
        Logger::Warning("[API] /arm: el guion no tiene MeasurePoint para el punto 1; "
                        "se usa control_api (" + std::to_string(camera) + ", " +
                        std::to_string(burstMs) + "/" + std::to_string(maxMs) + "ms)");
    }

    int spheres = 0;
    std::string motivo;
    if (!DoArmProbe(camera, burstMs, maxMs, spheres, motivo)) {
        status = 409;
        out = "{\"ok\":false,\"spheres\":" + std::to_string(spheres) +
              ",\"motivo\":\"" + JsonEscape(motivo) + "\"}";
        return;
    }

    if (spheres != Measure::RankingMeasure::kTargetSpheres) {
        status = 409;
        const std::string why = motivo.empty()
            ? ("se midieron " + std::to_string(spheres) + " esferas y se exigen " +
               std::to_string(Measure::RankingMeasure::kTargetSpheres))
            : motivo;
        out = "{\"ok\":false,\"spheres\":" + std::to_string(spheres) +
              ",\"motivo\":\"" + JsonEscape(why) + "\"}";
        Logger::Warning("[API] /arm rechazado: " + why);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_state = State::Armed;
        m_raceId = raceId;
        m_t0 = 0;
    }
    // Identidades limpias para la carrera: la sonda dejo tracks propios.
    m_measure->ResetIdentity();

    status = 200;
    out = "{\"ok\":true,\"spheres\":" + std::to_string(spheres) +
          ",\"race_id\":\"" + JsonEscape(raceId) + "\""
          ",\"camera\":" + std::to_string(camera) + "}";
    Logger::Info("[API] /arm OK race_id=" + raceId + " (10 esferas en la camara " +
                 std::to_string(camera) + "): state=armed");
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
    if (m_armInFlight.exchange(true)) {
        motivo = "ya hay un /arm en curso";
        return false;
    }
    struct InFlightGuard {
        std::atomic<bool>* flag;
        ~InFlightGuard() { flag->store(false); }
    } guard{&m_armInFlight};

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
