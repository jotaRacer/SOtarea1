# Planificador Dieciochero — Tarea 1, Sistemas Operativos

Simulador y planificador de actividades: lee un DAG de actividades desde un archivo de
texto y ejecuta cada actividad en su propio proceso hijo, con a lo más K procesos vivos
a la vez.

## 1. Integrantes

Matias Droguett 
Juan Pablo Ugaz 

## 2. Compilación y uso

### Compilar

Con el Makefile:

```bash
make
```

O a mano, con el comando equivalente:

```bash
g++ -Wall -Wextra -std=c++17 src/*.cpp -o planificador
```

El programa está dividido en varios archivos dentro de `src/`, así que hay que compilarlos
todos juntos (`src/*.cpp`). Compilar solo `main.cpp` falla porque faltan las otras partes.
Ambas formas generan el ejecutable `./planificador` en la raíz del proyecto.

### Ejecutar

```bash
./planificador plan.txt K
```

- `plan.txt`: archivo con el plan de actividades (formato en la sección 3). Puede tener
  cualquier nombre.
- `K`: máximo de procesos hijos simultáneos, un entero >= 1.

Ejemplo con el plan de prueba de 10.000 actividades:

```bash
./planificador plan10000test.txt 8
```

Para simular fallas, se puede definir la variable de entorno `FALLA_PROB`: la probabilidad,
entre 0 y 1, de que cada actividad falle (por defecto 0). La invocación no cambia:

```bash
FALLA_PROB=0.1 ./planificador plan10000test.txt 8
```

### Otros objetivos del Makefile

```bash
make debug    # recompila con -g -O0 -fsanitize=address,undefined
make clean    # borra el ejecutable
```

`make debug` deja un binario con sanitizers; para volver al normal: `make clean && make`.

Códigos de salida:

| Código | Significado |
|---|---|
| 0 | todas las actividades completadas |
| 1 | terminó, pero hubo actividades fallidas o abortadas |
| 2 | error de argumentos, de archivo, de plan o de `FALLA_PROB` |
| 130 | abortado por SIGINT (Ctrl+C) |

## 3. Formato del plan

Una actividad por línea: `ID : Nombre : tiempo_ms : dep1, dep2, ...`

```
1 : prender_carbon : 500 :
2 : comprar_carne : 1200 :
3 : comprar_pan : 300 :
4 : asar_longaniza : 800 : 1, 2
5 : armar_choripan : 250 : 3, 4
6 : servir_mesa : 100 : 5
```

- Los campos se separan por `:` y se recortan los espacios. Se aceptan 3 o 4 campos (el `:` final es opcional).
- **ID**: no vacío, solo `[A-Za-z0-9_]`. Es un string: `10` y `010` son IDs distintos.
- **Nombre**: no vacío.
- **tiempo_ms**: entero >= 0. Si está vacío, el padre sortea al parsear un valor uniforme en [100, 5000] ms, y en el log aparece marcado `(aleatoria)`.
- **Dependencias**: IDs separados por `,`. Una dependencia repetida en la misma línea se cuenta una vez, y puede estar declarada antes o después en el archivo.
- Se ignoran las líneas vacías, las que empiezan con `#` y el `\r` de los finales de línea de Windows.

Todo el plan se valida antes de crear cualquier proceso. Si hay errores, se reportan todos
(no solo el primero) en stderr con el formato `plan.txt:LINEA: mensaje` y el programa sale
con código 2 sin ejecutar nada. Se detectan: líneas mal formadas, tiempos inválidos (`abc`,
`-5`, `12.5`), IDs duplicados, dependencias inexistentes, autodependencias y ciclos (con la
lista de IDs involucrados). Un plan sin actividades es válido.

## 4. Diseño

**Padre planificador, un hijo por actividad.** El padre parsea el plan, mantiene el estado
de cada actividad (ESPERANDO → LISTA → EJECUTANDO → COMPLETADA | FALLIDA, o ABORTADA) y hace
`fork` por cada actividad lista. El hijo ejecuta código del mismo binario (sin `exec`): lee
sus insumos, duerme la duración con `nanosleep` y reporta su resultado. Separar la
planificación de la ejecución permite que una falla en un hijo nunca tumbe al padre.

**Cómo se respeta K.** Un hijo cuenta contra K desde el `fork` hasta que el padre lo recoge
con `waitpid`, y el padre solo hace `fork` si hay menos de K hijos vivos. Las actividades
listas sin cupo esperan en una cola FIFO. El resumen informa el máximo de procesos
simultáneos observado, como evidencia de que se respetó K.

**Relevo de mensajes por pipes.** No se crean procesos por adelantado: si se pre-crearan
todos, con 10.000 actividades se superaría K de inmediato. Por eso, cuando una actividad
termina, su dependiente aún no existe, y el padre hace de relevo con dos pipes por hijo:

1. *Hijo → padre*: antes de salir, el hijo escribe en una sola llamada a `write` un mensaje
   de a lo más 128 bytes (`<ID>:<nombre> listo en <ms> ms`).
2. *Padre → dependiente*: al lanzar una actividad, el padre escribe en su pipe de entrada una
   línea `INSUMO <ID_dep> <mensaje>` por cada dependencia y cierra el extremo de escritura.
   El hijo lee hasta EOF y loguea cada insumo (`RECIBE`) antes de simular.

El padre cierra los extremos que no usa justo después de cada `fork`, antes del siguiente.
Así ningún otro hijo los hereda, el EOF funciona y los descriptores abiertos quedan acotados
por K.

**`sigsuspend` y máscara de señales.** SIGINT y SIGCHLD se bloquean desde el inicio y solo
se reciben dentro de `sigsuspend`, cuyos handlers únicamente asignan un flag
`volatile sig_atomic_t`. Así se evita la carrera de revisar el flag de SIGINT justo antes de
bloquearse, y el padre no hace espera activa. En cada vuelta del bucle el padre revisa el
flag de SIGINT, recoge a todos los hijos terminados con `waitpid(-1, WNOHANG)`, lanza las
actividades que quepan y vuelve a `sigsuspend`.

**Aborto de ramas.** Si una actividad falla, el padre hace un BFS sobre sus dependientes y
marca cada uno como ABORTADA (causa `depende de <ID>`), aunque tenga otras dependencias
sanas. Ninguno puede estar corriendo, porque una actividad solo se lanza cuando todas sus
dependencias se completaron. El resto del plan sigue normalmente.

**SIGINT (Inspección de la Seremi).** Los hijos ignoran SIGINT, así que un Ctrl+C, que llega
a todo el grupo de procesos, solo lo atiende el padre: envía SIGKILL a cada hijo vivo, los
recoge con `waitpid`, marca como ABORTADAS las actividades en ejecución y las pendientes
(de estas últimas solo loguea la cantidad), imprime el resumen y sale con 130. Un segundo
Ctrl+C no cambia nada, porque SIGINT sigue bloqueado y el flag ya está puesto.

**Log.** Padre e hijos escriben directo en stdout: cada línea se arma con `snprintf` y se
emite con un solo `write` de a lo más 512 bytes, sin buffers de stdio, para que las líneas
no se mezclen ni se dupliquen al hacer `fork`. El timestamp es relativo al inicio del
programa (`CLOCK_MONOTONIC`).

```
[0000000] CARGA 6 actividades, K=2, FALLA_PROB=0
[0000000] INICIO 1 prender_carbon pid=31701 dur=500ms
[0000000] INICIO 2 comprar_carne pid=31702 dur=1200ms
[0000501] FIN 1 prender_carbon: 1:prender_carbon listo en 500 ms
[0000501] INICIO 3 comprar_pan pid=31703 dur=300ms
...
[0001201] INICIO 4 asar_longaniza pid=31706 dur=800ms
[0001201] RECIBE 4 <- 1: 1:prender_carbon listo en 500 ms
[0001201] RECIBE 4 <- 2: 2:comprar_carne listo en 1200 ms
...
===== RESUMEN =====
Total: 6 | Completadas: 6 | Fallidas: 0 | Abortadas: 0
Tiempo total: 2357 ms | Máx. procesos simultáneos: 2 (K=2)
```

Organización del código:

| Archivo | Contenido |
|---|---|
| `src/main.cpp` | argumentos, `FALLA_PROB`, instalación de señales, arranque |
| `src/plan.cpp` | parseo, validación y detección de ciclos (Kahn), todo O(N + E) |
| `src/scheduler.cpp` | bucle del padre: lanzamiento, recolección, abortos, SIGINT, resumen |
| `src/worker.cpp` | código que corre en el hijo |
| `src/log.cpp` | salida con timestamp y un solo `write` por línea |

## 5. Supuestos

- **Qué cuenta en K**: todo hijo creado y aún no recogido con `waitpid`, incluidos los que ya
  terminaron pero siguen como zombie. El proceso padre no cuenta.
- **Qué es una falla**: un hijo que termina con código distinto de 0, muere por una señal
  (por ejemplo `kill -9 <pid>` desde otra terminal) o no envía mensaje. En el log aparece
  como `FALLA` con causa `exit N`, `señal N (nombre)` o `terminó sin enviar mensaje`.
- **Inyección de fallas**: la variable de entorno `FALLA_PROB` (decimal entre 0 y 1, por
  defecto 0) permite demostrar fallas sin cambiar la invocación. Cada hijo, con esa
  probabilidad, duerme una fracción aleatoria de su duración y termina con `_exit(1)` sin
  escribir mensaje. Un valor inválido es error de argumentos (código 2).
- **Orden**: las actividades listas se lanzan en orden FIFO; las iniciales, en orden de
  archivo. No hay prioridades.
- **Validación previa**: el plan completo se valida antes de crear procesos, así que un plan
  con errores no ejecuta nada.
- **Mensaje "acotado"**: a lo más 128 bytes; si el nombre no cabe, se trunca.
- **Duraciones aleatorias**: se sortean en el padre al parsear (`std::mt19937` sembrado con
  `time(NULL) ^ getpid()`), para que queden fijas y aparezcan en el log.
- **Falla al crear procesos**: si `fork` o `pipe` fallan por falta de recursos, el nodo
  vuelve al frente de la cola y el padre espera a que termine un hijo. Si no hay hijos
  vivos, reintenta cada 100 ms hasta 50 veces y luego marca el nodo FALLIDA (`no se pudo
  crear el proceso`) y aborta su rama. Esto se aplica a cualquier error de `fork`/`pipe`,
  no solo a `EAGAIN`/`EMFILE`.
- **Causa del aborto**: en `ABORTADA ... depende de X`, X es la actividad que falló (la raíz
  de la rama abortada), no necesariamente la dependencia directa.
- **Líneas del log**: se truncan a 512 bytes, el `PIPE_BUF` de macOS, para que cada una se
  escriba de forma indivisible.

## 6. Pruebas

Se probó con `plan10000test.txt`, un plan de 10.000 actividades de 1–4 ms con 19.040
dependencias, y K = 1, 8, 64 y 512. En todos los casos se completaron las 10.000
actividades y el máximo de procesos simultáneos no superó K. Para reproducir las pruebas
a mano:

```bash
make

# Ejecución normal: código 0 y "Máx. procesos simultáneos" <= K en el resumen
./planificador plan10000test.txt 8; echo $?

# Fallas inyectadas: código 1; cada FALLA viene seguida de las ABORTADA de su rama
FALLA_PROB=0.05 ./planificador plan10000test.txt 64; echo $?

# Ctrl+C: presionarlo mientras corre. Código 130, resumen impreso y, después,
# `pgrep -x planificador` no muestra procesos vivos
./planificador plan10000test.txt 1

# kill -9 a un hijo: mientras corre, desde otra terminal, matar el pid de una línea
# INICIO reciente. Aparece como "FALLA ... señal 9" y solo se aborta su rama
./planificador plan10000test.txt 1

# Argumentos inválidos: código 2
./planificador plan10000test.txt 0; echo $?
```

## 7. Limitaciones conocidas

- **Límites del sistema**: cada hijo vivo usa un proceso y el padre mantiene un descriptor
  abierto por hijo. Con K muy grande se pueden alcanzar `ulimit -u` (procesos por usuario)
  o `ulimit -n` (descriptores; en macOS el valor por defecto suele ser 256). En ese caso
  `fork`/`pipe` fallan y el planificador espera a que termine un hijo antes de reintentar,
  por lo que el máximo simultáneo real puede quedar bajo K. Nunca se supera K.
- Si nunca se logra crear un proceso (sin hijos vivos, 50 reintentos), la actividad se
  marca FALLIDA en vez de esperar indefinidamente.
- Tras SIGINT, las actividades en ejecución se marcan ABORTADAS aunque justo hayan
  terminado antes de ser recogidas.
- Un hijo hereda los extremos de lectura de los pipes de resultado de sus hermanos
  vivos (a lo más K − 1). No afecta el EOF ni la corrección, pero ocupa descriptores.
