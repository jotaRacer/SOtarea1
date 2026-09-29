#pragma once

#include <csignal>
#include <cstddef>

#include "plan.hpp"

// Largo máximo del mensaje de resultado que el hijo envía al padre.
constexpr std::size_t MSG_MAX = 128;

// Código del hijo tras el fork: lee y loguea los insumos de in_fd hasta EOF,
// simula la actividad (o falla según falla_prob) y escribe su resultado en out_fd.
// Nunca retorna: termina siempre con _exit.
[[noreturn]] void ejecutar_worker(const Nodo& nodo, int in_fd, int out_fd, double falla_prob,
                                  const sigset_t& mascara_original);
