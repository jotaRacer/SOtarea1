#include "log.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <unistd.h>

namespace {

timespec g_inicio;

// 512 bytes = PIPE_BUF de macOS (en Linux es 4096). Una escritura de ese tamaño
// a un pipe no se intercala con las de otros procesos.
constexpr std::size_t LINEA_MAX = 512;

void emitir(int fd, bool con_timestamp, const char* fmt, va_list ap) {
    char buf[LINEA_MAX];
    int off = 0;
    if (con_timestamp) off = std::snprintf(buf, sizeof buf, "[%07ld] ", log_ms());
    // Se reserva un byte para el '\n' final; si el texto no cabe, se trunca.
    std::size_t espacio = sizeof buf - static_cast<std::size_t>(off) - 1;
    int n = std::vsnprintf(buf + off, espacio, fmt, ap);
    if (n < 0) return;
    std::size_t len = static_cast<std::size_t>(off) + std::min(static_cast<std::size_t>(n), espacio - 1);
    buf[len++] = '\n';
    escribir_todo(fd, buf, len);
}

}  // namespace

void log_init() { clock_gettime(CLOCK_MONOTONIC, &g_inicio); }

long log_ms() {
    timespec ahora;
    clock_gettime(CLOCK_MONOTONIC, &ahora);
    return (ahora.tv_sec - g_inicio.tv_sec) * 1000L + (ahora.tv_nsec - g_inicio.tv_nsec) / 1000000L;
}

void escribir_todo(int fd, const char* buf, std::size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        buf += n;
        len -= static_cast<std::size_t>(n);
    }
}

void log_evento(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emitir(STDOUT_FILENO, true, fmt, ap);
    va_end(ap);
}

void log_linea(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emitir(STDOUT_FILENO, false, fmt, ap);
    va_end(ap);
}

void log_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emitir(STDERR_FILENO, false, fmt, ap);
    va_end(ap);
}
