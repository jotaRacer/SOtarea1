#pragma once

#include <cstddef>

// Guarda el instante inicial (CLOCK_MONOTONIC). Se llama antes del primer fork
// para que los hijos hereden la misma referencia.
void log_init();

// Milisegundos transcurridos desde log_init().
long log_ms();

// Escribe len bytes en fd, reintentando ante EINTR o escrituras parciales.
void escribir_todo(int fd, const char* buf, std::size_t len);

// Evento en stdout: "[<ms, 7 dígitos>] <texto>\n", emitido con un solo write.
void log_evento(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Línea en stdout sin timestamp (resumen final).
void log_linea(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Línea en stderr sin timestamp (errores de argumentos y de plan).
void log_error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
