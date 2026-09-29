#include <cctype>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <string>
#include <vector>

#include "log.hpp"
#include "plan.hpp"
#include "scheduler.hpp"

namespace {

void manejar_sigint(int) { g_sigint = 1; }
void manejar_sigchld(int) { g_sigchld = 1; }

bool parsear_k(const char* s, int& k) {
    if (*s == '\0') return false;
    for (const char* p = s; *p; ++p)
        if (!std::isdigit(static_cast<unsigned char>(*p))) return false;
    errno = 0;
    long v = std::strtol(s, nullptr, 10);
    if (errno == ERANGE || v < 1 || v > INT_MAX) return false;
    k = static_cast<int>(v);
    return true;
}

bool parsear_prob(const char* s, double& p) {
    char* fin = nullptr;
    errno = 0;
    p = std::strtod(s, &fin);
    return fin != s && *fin == '\0' && errno != ERANGE && p >= 0.0 && p <= 1.0;
}

// SIGINT y SIGCHLD quedan bloqueadas desde el inicio y solo se reciben dentro de
// sigsuspend; los handlers únicamente asignan un flag.
void instalar_senales(sigset_t& mascara_original) {
    sigset_t bloqueo;
    sigemptyset(&bloqueo);
    sigaddset(&bloqueo, SIGINT);
    sigaddset(&bloqueo, SIGCHLD);
    sigprocmask(SIG_BLOCK, &bloqueo, &mascara_original);

    struct sigaction sa{};
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = manejar_sigint;
    sigaction(SIGINT, &sa, nullptr);
    sa.sa_handler = manejar_sigchld;
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    log_init();

    int k = 0;
    if (argc != 3 || !parsear_k(argv[2], k)) {
        log_error("uso: ./planificador plan.txt K");
        return 2;
    }
    double falla_prob = 0.0;
    const char* env = std::getenv("FALLA_PROB");
    if (env != nullptr && !parsear_prob(env, falla_prob)) {
        log_error("FALLA_PROB inválido '%s': debe ser un decimal entre 0 y 1", env);
        return 2;
    }

    sigset_t mascara_original;
    instalar_senales(mascara_original);

    Plan plan;
    std::vector<std::string> errores;
    if (!cargar_plan(argv[1], plan, errores)) {
        for (const std::string& e : errores) log_error("%s", e.c_str());
        return 2;
    }
    return ejecutar_plan(plan, k, falla_prob, mascara_original);
}
