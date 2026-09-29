#include "plan.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <random>
#include <unordered_set>
#include <unistd.h>

namespace {

struct Error {
    int linea;
    std::string msg;
};

std::string recortar(const std::string& s) {
    std::size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    std::size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> separar(const std::string& s, char sep) {
    std::vector<std::string> partes;
    std::size_t ini = 0;
    while (true) {
        std::size_t p = s.find(sep, ini);
        partes.push_back(recortar(s.substr(ini, p == std::string::npos ? std::string::npos : p - ini)));
        if (p == std::string::npos) break;
        ini = p + 1;
    }
    return partes;
}

bool id_valido(const std::string& id) {
    if (id.empty()) return false;
    for (unsigned char c : id)
        if (!std::isalnum(c) && c != '_') return false;
    return true;
}

bool parsear_tiempo(const std::string& s, long& out) {
    for (unsigned char c : s)
        if (!std::isdigit(c)) return false;
    errno = 0;
    out = std::strtol(s.c_str(), nullptr, 10);
    return errno != ERANGE;
}

// Kahn: devuelve los nodos que quedan con grado de entrada > 0. Luego se descartan
// los que solo dependen de un ciclo (sin salida dentro del resto), para listar
// únicamente los que forman ciclos. Todo en O(N + E).
std::vector<int> nodos_en_ciclo(const std::vector<Nodo>& nodos) {
    const std::size_t n = nodos.size();
    std::vector<int> grado(n);
    std::vector<int> cola;
    for (std::size_t i = 0; i < n; ++i) {
        grado[i] = static_cast<int>(nodos[i].padres.size());
        if (grado[i] == 0) cola.push_back(static_cast<int>(i));
    }
    for (std::size_t h = 0; h < cola.size(); ++h)
        for (int v : nodos[cola[h]].hijos)
            if (--grado[v] == 0) cola.push_back(v);
    if (cola.size() == n) return {};

    std::vector<char> resto(n);
    std::vector<int> salida(n, 0);
    for (std::size_t i = 0; i < n; ++i) resto[i] = grado[i] > 0;
    cola.clear();
    for (std::size_t i = 0; i < n; ++i) {
        if (!resto[i]) continue;
        for (int v : nodos[i].hijos)
            if (resto[v]) ++salida[i];
        if (salida[i] == 0) cola.push_back(static_cast<int>(i));
    }
    for (std::size_t h = 0; h < cola.size(); ++h) {
        int u = cola[h];
        resto[u] = 0;
        for (int p : nodos[u].padres)
            if (resto[p] && --salida[p] == 0) cola.push_back(p);
    }
    std::vector<int> ciclo;
    for (std::size_t i = 0; i < n; ++i)
        if (resto[i]) ciclo.push_back(static_cast<int>(i));
    return ciclo;
}

}  // namespace

bool cargar_plan(const std::string& ruta, Plan& plan, std::vector<std::string>& errores) {
    std::ifstream in(ruta);
    if (!in) {
        errores.push_back(ruta + ": no se pudo abrir el archivo: " + std::strerror(errno));
        return false;
    }

    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) ^ static_cast<unsigned>(getpid()));
    std::uniform_int_distribution<long> sorteo(100, 5000);

    std::vector<Error> errs;
    std::vector<std::vector<std::string>> deps_texto;  // por nodo, en orden de archivo
    std::string linea;
    int nlinea = 0;
    while (std::getline(in, linea)) {
        ++nlinea;
        std::string t = recortar(linea);
        if (t.empty() || t[0] == '#') continue;

        std::vector<std::string> campos = separar(t, ':');
        if (campos.size() < 3 || campos.size() > 4) {
            errs.push_back({nlinea, "línea mal formada: se esperan 3 o 4 campos separados por ':'"});
            continue;
        }
        const std::string& id = campos[0];
        const std::string& nombre = campos[1];
        const std::string& tiempo = campos[2];

        bool id_ok = id_valido(id);
        if (id.empty())
            errs.push_back({nlinea, "línea mal formada: ID vacío"});
        else if (!id_ok)
            errs.push_back({nlinea, "línea mal formada: ID '" + id + "' con caracteres no permitidos (solo A-Z, a-z, 0-9, _)"});
        if (nombre.empty()) errs.push_back({nlinea, "línea mal formada: nombre vacío"});

        long dur = 0;
        bool aleatoria = false;
        if (tiempo.empty()) {
            dur = sorteo(rng);
            aleatoria = true;
        } else if (!parsear_tiempo(tiempo, dur)) {
            errs.push_back({nlinea, "tiempo inválido '" + tiempo + "': debe ser un entero >= 0"});
        }

        std::vector<std::string> deps;
        if (campos.size() == 4 && !campos[3].empty()) {
            std::unordered_set<std::string> vistas;
            bool autodep = false;
            for (const std::string& d : separar(campos[3], ',')) {
                if (d.empty()) {
                    errs.push_back({nlinea, "línea mal formada: dependencia vacía en la lista"});
                    break;
                }
                if (d == id) {
                    if (!autodep) errs.push_back({nlinea, "autodependencia: '" + id + "' depende de sí misma"});
                    autodep = true;
                    continue;
                }
                if (vistas.insert(d).second) deps.push_back(d);
            }
        }

        // El nodo se registra aunque otros campos tengan errores, para no reportar
        // en cascada "dependencia inexistente" en las líneas que lo usan.
        if (!id_ok) continue;
        auto previo = plan.indice.find(id);
        if (previo != plan.indice.end()) {
            errs.push_back({nlinea, "ID duplicado '" + id + "' (ya definido en la línea " +
                                        std::to_string(plan.nodos[previo->second].linea) + ")"});
            continue;
        }
        plan.indice.emplace(id, static_cast<int>(plan.nodos.size()));
        Nodo nodo;
        nodo.id = id;
        nodo.nombre = nombre;
        nodo.dur_ms = dur;
        nodo.aleatoria = aleatoria;
        nodo.linea = nlinea;
        plan.nodos.push_back(std::move(nodo));
        deps_texto.push_back(std::move(deps));
    }
    if (in.bad()) {
        errores.push_back(ruta + ": error al leer el archivo");
        return false;
    }

    for (std::size_t i = 0; i < plan.nodos.size(); ++i) {
        for (const std::string& d : deps_texto[i]) {
            auto it = plan.indice.find(d);
            if (it == plan.indice.end()) {
                errs.push_back({plan.nodos[i].linea, "dependencia inexistente '" + d + "'"});
                continue;
            }
            plan.nodos[i].padres.push_back(it->second);
            plan.nodos[it->second].hijos.push_back(static_cast<int>(i));
        }
        plan.nodos[i].pendientes = static_cast<int>(plan.nodos[i].padres.size());
    }

    std::vector<int> ciclo = nodos_en_ciclo(plan.nodos);
    if (!ciclo.empty()) {
        std::string ids;
        for (int v : ciclo) ids += (ids.empty() ? "" : ", ") + plan.nodos[v].id;
        errs.push_back({plan.nodos[ciclo[0]].linea, "ciclo entre las actividades: " + ids});
    }

    std::stable_sort(errs.begin(), errs.end(), [](const Error& a, const Error& b) { return a.linea < b.linea; });
    for (const Error& e : errs) errores.push_back(ruta + ":" + std::to_string(e.linea) + ": " + e.msg);
    return errs.empty();
}
