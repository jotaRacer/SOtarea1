#include "worker.hpp"

#include <cerrno>
#include <ctime>
#include <random>
#include <string>
#include <unistd.h>

#include "log.hpp"

namespace {

std::string leer_hasta_eof(int fd) {
    std::string datos;
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        datos.append(buf, static_cast<std::size_t>(n));
    }
    return datos;
}

// Cada línea tiene la forma "INSUMO <ID_dep> <mensaje>".
void loguear_insumos(const Nodo& nodo, const std::string& datos) {
    const std::string prefijo = "INSUMO ";
    std::size_t ini = 0;
    while (ini < datos.size()) {
        std::size_t fin = datos.find('\n', ini);
        if (fin == std::string::npos) fin = datos.size();
        std::string linea = datos.substr(ini, fin - ini);
        ini = fin + 1;
        if (linea.compare(0, prefijo.size(), prefijo) != 0) continue;
        std::size_t esp = linea.find(' ', prefijo.size());
        std::string dep = linea.substr(prefijo.size(), esp == std::string::npos ? std::string::npos : esp - prefijo.size());
        std::string msg = esp == std::string::npos ? "" : linea.substr(esp + 1);
        log_evento("RECIBE %s <- %s: %s", nodo.id.c_str(), dep.c_str(), msg.c_str());
    }
}

void dormir_ms(long ms) {
    timespec t{ms / 1000, (ms % 1000) * 1000000L};
    timespec resto;
    while (nanosleep(&t, &resto) < 0 && errno == EINTR) t = resto;
}

// "<ID>:<nombre> listo en <ms> ms", de a lo más MSG_MAX bytes. Si no cabe se trunca
// el nombre (y, si el ID solo ya es muy largo, el final del mensaje).
std::string armar_mensaje(const Nodo& nodo) {
    std::string prefijo = nodo.id + ":";
    std::string sufijo = " listo en " + std::to_string(nodo.dur_ms) + " ms";
    std::size_t fijo = prefijo.size() + sufijo.size();
    std::size_t cabe = MSG_MAX > fijo ? MSG_MAX - fijo : 0;
    std::string msg = prefijo + nodo.nombre.substr(0, cabe) + sufijo;
    if (msg.size() > MSG_MAX) msg.resize(MSG_MAX);
    return msg;
}

}  // namespace

void ejecutar_worker(const Nodo& nodo, int in_fd, int out_fd, double falla_prob, const sigset_t& mascara_original) {
    // Ctrl+C llega a todo el grupo; el hijo lo ignora y el padre controla el cierre.
    struct sigaction sa{};
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGINT, &sa, nullptr);
    sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &sa, nullptr);
    sigprocmask(SIG_SETMASK, &mascara_original, nullptr);

    loguear_insumos(nodo, leer_hasta_eof(in_fd));
    close(in_fd);

    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(getpid()));
    std::uniform_real_distribution<double> u(0.0, 1.0);
    if (u(rng) < falla_prob) {
        dormir_ms(static_cast<long>(static_cast<double>(nodo.dur_ms) * u(rng)));
        _exit(1);
    }

    dormir_ms(nodo.dur_ms);
    std::string msg = armar_mensaje(nodo);
    if (write(out_fd, msg.data(), msg.size()) != static_cast<ssize_t>(msg.size())) _exit(1);
    _exit(0);
}
