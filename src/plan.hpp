#pragma once

#include <string>
#include <unordered_map>
#include <vector>

enum class Estado { ESPERANDO, LISTA, EJECUTANDO, COMPLETADA, FALLIDA, ABORTADA };

struct Nodo {
    std::string id;
    std::string nombre;
    long dur_ms = 0;
    bool aleatoria = false;     // la duración se sorteó porque venía vacía
    int linea = 0;              // línea del archivo, para los mensajes de error
    std::vector<int> padres;    // dependencias
    std::vector<int> hijos;     // dependientes
    int pendientes = 0;         // dependencias que aún no terminan
    Estado estado = Estado::ESPERANDO;
    std::string mensaje;        // resultado recibido del hijo al completar
};

struct Plan {
    std::vector<Nodo> nodos;    // en orden de archivo
    std::unordered_map<std::string, int> indice;
};

// Lee y valida el plan completo. Devuelve false si el archivo no se pudo leer o si
// hay errores; en ese caso `errores` trae todos los mensajes ("ruta:LINEA: mensaje").
bool cargar_plan(const std::string& ruta, Plan& plan, std::vector<std::string>& errores);
