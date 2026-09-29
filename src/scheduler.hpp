#pragma once

#include <csignal>

#include "plan.hpp"

// Flags que asignan los handlers de SIGINT y SIGCHLD (instalados en main).
extern volatile sig_atomic_t g_sigint;
extern volatile sig_atomic_t g_sigchld;

// Ejecuta el plan con a lo más k hijos vivos. Se llama con SIGINT y SIGCHLD
// bloqueados; mascara_original es la máscara previa, que se usa en sigsuspend y
// que los hijos restauran. Devuelve el código de salida del programa.
int ejecutar_plan(Plan& plan, int k, double falla_prob, const sigset_t& mascara_original);
