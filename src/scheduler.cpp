#include "scheduler.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>

#include "log.hpp"
#include "worker.hpp"

volatile sig_atomic_t g_sigint = 0;
volatile sig_atomic_t g_sigchld = 0;

namespace {

constexpr int REINTENTOS_MAX = 50;
constexpr long ESPERA_REINTENTO_NS = 100 * 1000000L;  // 100 ms

struct Hijo {
    int nodo;
    int fd;  // extremo de lectura de su out_pipe
};

enum class Lanzamiento { OK, SIN_RECURSOS };

class Planificador {
public:
    Planificador(Plan& plan, int k, double falla_prob, const sigset_t& mascara_original)
        : plan_(plan), k_(static_cast<std::size_t>(k)), falla_prob_(falla_prob), mascara_original_(mascara_original) {}

    int ejecutar() {
        log_evento("CARGA %zu actividades, K=%zu, FALLA_PROB=%g", plan_.nodos.size(), k_, falla_prob_);
        for (std::size_t i = 0; i < plan_.nodos.size(); ++i) {
            if (plan_.nodos[i].pendientes == 0) {
                plan_.nodos[i].estado = Estado::LISTA;
                listos_.push_back(static_cast<int>(i));
            }
        }

        bool interrumpido = false;
        while (true) {
            if (g_sigint) {
                abortar_todo();
                interrumpido = true;
                break;
            }
            recoger_hijos();
            bool reintentar = lanzar_listos();
            if (vivos_.empty() && listos_.empty()) break;
            if (!vivos_.empty())
                sigsuspend(&mascara_original_);  // despierta con SIGCHLD o SIGINT
            else if (reintentar)
                esperar_reintento();
        }

        imprimir_resumen();
        if (interrumpido) return 130;
        return contar(Estado::COMPLETADA) == plan_.nodos.size() ? 0 : 1;
    }

private:
    // Lanza nodos de la cola mientras haya cupo. Devuelve true si no se pudo crear un
    // proceso y no hay hijos vivos que esperar (el bucle debe dormir y reintentar).
    bool lanzar_listos() {
        while (vivos_.size() < k_ && !listos_.empty()) {
            int n = listos_.front();
            listos_.pop_front();
            if (lanzar(n) == Lanzamiento::OK) {
                reintentos_ = 0;
                continue;
            }
            if (!vivos_.empty()) {  // esperar a que termine un hijo y libere recursos
                listos_.push_front(n);
                return false;
            }
            if (++reintentos_ >= REINTENTOS_MAX) {
                reintentos_ = 0;
                fallar(n, "no se pudo crear el proceso");
                continue;
            }
            listos_.push_front(n);
            return true;
        }
        return false;
    }

    Lanzamiento lanzar(int n) {
        Nodo& nodo = plan_.nodos[n];
        int in_pipe[2], out_pipe[2];
        if (pipe(in_pipe) < 0) return Lanzamiento::SIN_RECURSOS;
        if (pipe(out_pipe) < 0) {
            close(in_pipe[0]);
            close(in_pipe[1]);
            return Lanzamiento::SIN_RECURSOS;
        }
        pid_t pid = fork();
        if (pid < 0) {
            close(in_pipe[0]);
            close(in_pipe[1]);
            close(out_pipe[0]);
            close(out_pipe[1]);
            return Lanzamiento::SIN_RECURSOS;
        }
        if (pid == 0) {
            close(in_pipe[1]);
            close(out_pipe[0]);
            ejecutar_worker(nodo, in_pipe[0], out_pipe[1], falla_prob_, mascara_original_);
        }

        // Se cierran antes del siguiente fork para que ningún otro hijo los herede.
        close(in_pipe[0]);
        close(out_pipe[1]);
        nodo.estado = Estado::EJECUTANDO;
        vivos_[pid] = Hijo{n, out_pipe[0]};
        max_vivos_ = std::max(max_vivos_, vivos_.size());
        log_evento("INICIO %s %s pid=%d dur=%ldms%s", nodo.id.c_str(), nodo.nombre.c_str(), static_cast<int>(pid),
                   nodo.dur_ms, nodo.aleatoria ? " (aleatoria)" : "");
        enviar_insumos(nodo, in_pipe[1]);
        close(in_pipe[1]);
        return Lanzamiento::OK;
    }

    // Si el hijo murió antes de leer, write falla con EPIPE (SIGPIPE está ignorada).
    void enviar_insumos(const Nodo& nodo, int fd) {
        std::string datos;
        for (int p : nodo.padres) {
            const Nodo& dep = plan_.nodos[p];
            datos += "INSUMO " + dep.id + " " + dep.mensaje + "\n";
        }
        escribir_todo(fd, datos.data(), datos.size());
    }

    void recoger_hijos() {
        int status;
        pid_t pid;
        while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
            auto it = vivos_.find(pid);
            if (it == vivos_.end()) continue;
            Hijo hijo = it->second;
            vivos_.erase(it);
            std::string msg = leer_mensaje(hijo.fd);
            close(hijo.fd);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0 && !msg.empty())
                completar(hijo.nodo, msg);
            else
                fallar(hijo.nodo, causa_falla(status));
        }
    }

    static std::string leer_mensaje(int fd) {
        char buf[MSG_MAX];
        std::size_t total = 0;
        while (total < sizeof buf) {
            ssize_t n = read(fd, buf + total, sizeof buf - total);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            total += static_cast<std::size_t>(n);
        }
        return std::string(buf, total);
    }

    static std::string causa_falla(int status) {
        char buf[128];
        if (WIFSIGNALED(status))
            std::snprintf(buf, sizeof buf, "señal %d (%s)", WTERMSIG(status), strsignal(WTERMSIG(status)));
        else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            std::snprintf(buf, sizeof buf, "exit %d", WEXITSTATUS(status));
        else
            return "terminó sin enviar mensaje";
        return buf;
    }

    void completar(int n, const std::string& msg) {
        Nodo& nodo = plan_.nodos[n];
        nodo.estado = Estado::COMPLETADA;
        nodo.mensaje = msg;
        log_evento("FIN %s %s: %s", nodo.id.c_str(), nodo.nombre.c_str(), msg.c_str());
        for (int d : nodo.hijos) {
            Nodo& dep = plan_.nodos[d];
            if (--dep.pendientes == 0 && dep.estado == Estado::ESPERANDO) {
                dep.estado = Estado::LISTA;
                listos_.push_back(d);
            }
        }
    }

    void fallar(int n, const std::string& causa) {
        Nodo& nodo = plan_.nodos[n];
        nodo.estado = Estado::FALLIDA;
        log_evento("FALLA %s %s: %s", nodo.id.c_str(), nodo.nombre.c_str(), causa.c_str());
        abortar_rama(n);
    }

    // BFS sobre los dependientes de f. Ninguno puede estar en ejecución, porque un
    // nodo solo se lanza con todas sus dependencias completadas.
    void abortar_rama(int f) {
        const Nodo& fallido = plan_.nodos[f];
        std::vector<int> cola(fallido.hijos);
        for (std::size_t h = 0; h < cola.size(); ++h) {
            Nodo& nodo = plan_.nodos[cola[h]];
            if (nodo.estado == Estado::ABORTADA) continue;
            nodo.estado = Estado::ABORTADA;
            log_evento("ABORTADA %s %s: depende de %s", nodo.id.c_str(), nodo.nombre.c_str(), fallido.id.c_str());
            for (int d : nodo.hijos)
                if (plan_.nodos[d].estado != Estado::ABORTADA) cola.push_back(d);
        }
    }

    void abortar_todo() {
        log_evento("INSPECCION SEREMI: abortando todas las actividades");
        for (const auto& par : vivos_) kill(par.first, SIGKILL);
        for (const auto& par : vivos_) {
            int status;
            while (waitpid(par.first, &status, 0) < 0 && errno == EINTR) {
            }
            close(par.second.fd);
            Nodo& nodo = plan_.nodos[par.second.nodo];
            nodo.estado = Estado::ABORTADA;
            log_evento("ABORTADA %s %s: inspección, en ejecución", nodo.id.c_str(), nodo.nombre.c_str());
        }
        vivos_.clear();
        listos_.clear();

        std::size_t pendientes = 0;
        for (Nodo& nodo : plan_.nodos) {
            if (nodo.estado == Estado::ESPERANDO || nodo.estado == Estado::LISTA) {
                nodo.estado = Estado::ABORTADA;
                ++pendientes;
            }
        }
        log_evento("INSPECCION %zu actividades pendientes abortadas", pendientes);
    }

    // Sin hijos vivos no llegará SIGCHLD: se desbloquean las señales durante la
    // espera para que un Ctrl+C la interrumpa.
    void esperar_reintento() {
        sigset_t bloqueadas;
        sigprocmask(SIG_SETMASK, &mascara_original_, &bloqueadas);
        timespec t{0, ESPERA_REINTENTO_NS};
        nanosleep(&t, nullptr);
        sigprocmask(SIG_SETMASK, &bloqueadas, nullptr);
    }

    std::size_t contar(Estado e) const {
        return static_cast<std::size_t>(
            std::count_if(plan_.nodos.begin(), plan_.nodos.end(), [e](const Nodo& n) { return n.estado == e; }));
    }

    void imprimir_resumen() const {
        log_linea("===== RESUMEN =====");
        log_linea("Total: %zu | Completadas: %zu | Fallidas: %zu | Abortadas: %zu", plan_.nodos.size(),
                  contar(Estado::COMPLETADA), contar(Estado::FALLIDA), contar(Estado::ABORTADA));
        log_linea("Tiempo total: %ld ms | Máx. procesos simultáneos: %zu (K=%zu)", log_ms(), max_vivos_, k_);
    }

    Plan& plan_;
    const std::size_t k_;
    const double falla_prob_;
    const sigset_t mascara_original_;
    std::unordered_map<pid_t, Hijo> vivos_;
    std::deque<int> listos_;
    std::size_t max_vivos_ = 0;
    int reintentos_ = 0;
};

}  // namespace

int ejecutar_plan(Plan& plan, int k, double falla_prob, const sigset_t& mascara_original) {
    return Planificador(plan, k, falla_prob, mascara_original).ejecutar();
}
