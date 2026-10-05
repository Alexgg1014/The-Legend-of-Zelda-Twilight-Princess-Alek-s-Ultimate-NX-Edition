# H37: cargador dinámico de mods nativos, seguridad, Randomizer y arenas de CodeMemory

Base: `claude/h36-mod-platform` en `abbf6b9887fb695cc9a4e146436a593ca4bd96b7`; Aurora
`845a511c5b33588118554be2b410734f8c2cf2eb`; Cosmetics (`93971c38…`) y Randomizer
(`1f24e81303536db4b77f49d057a8ec1f11162000`) en sus gitlinks de H36, **sin cambios**. Rama:
`claude/h37-dynamic-mod-loader`. Las ramas H35 y H36 no se tocan; ningún submódulo cambia.

Nada de esto se ha ejecutado en una Switch: todo lo que se afirma como probado lo está en host (x86-64 con
sanitizers, o aarch64 bajo qemu-user). La §13 dice qué solo puede comprobarse en consola.

## 0. Hallazgos de hardware y de H36 incorporados en H37

### 0.1 Bloqueo de hardware: `svcCreateCodeMemory failed (0xce01)`

H36-c1 (`H36-c1-abbf6b9887`) en consola: Cosmetics instaló 10 hooks y el 11.º falló con
`funchook failed for 0x528eeef40 (prepare=8 install=-1): svcCreateCodeMemory failed (0xce01)` — módulo 1 (Kernel),
descripción 103, `KernelError_ResourceExhausted`. El backend de funchook para Horizon creaba **un objeto CodeMemory
por página de trampolín**; Cosmetics necesita 28, Roll and Sprint 4, Randomizer 92. Tras el rollback de Cosmetics,
Roll and Sprint instaló sus 4 hooks: los objetos se liberan, el límite es del kernel, no una fuga.

El límite observado (10) coincide con el tamaño del pool de objetos KCodeMemory del kernel tal como lo reproduce
mesosphere; no lo he podido verificar aquí, y no hace falta: el diseño nuevo usa 1–2 objetos en total y la prueba
simula un kernel que rechaza el 11.º. **§6** describe la solución. Aumentar `MAX_CODE_PAGES` no habría servido.

### 0.2 `.dusk` canónicos entre plataformas

| checkout | SHA-256 de `luau_probe.dusk` |
|---|---|
| H36, LF (nube) | `9c8a996c80070251e7f45cf1930d235e5ef70e39cd7a57dfbc2c7e8f61eb279f` |
| H36, CRLF (Windows) | `c67b708ee5c8a0ceb8dbbe9192ae6df9e330cf54c587f043b4e6f95ff06c92ae` |
| **H37, cualquier checkout y host** | **`a6f964fe8a39591b7473ca67a92aaa892db705e01e162b62d375bd2ed5a30004`** |

Causas, las dos comprobadas: (1) Git entregaba los ficheros de texto con CRLF y `make_dusk.py` empaquetaba los bytes
tal cual; (2) la salida de deflate depende de la implementación de zlib: la misma carpeta LF da `9c8a996c…` con
zlib 1.3 y `6956f4ba…` con zlib-ng (el de los Python recientes de Windows). Ninguna de las 16 combinaciones LF/CRLF
reproduce `c67b708e…` con zlib de Linux, así que en Windows intervinieron ambas.

`make_dusk.py` empaqueta ahora de forma canónica: entradas **sin comprimir** (`stored`), ficheros de texto (por
extensión: json, luau, txt, rml, rcss, yaml, fuentes…) **con LF**, el resto byte a byte, entradas **ordenadas por
nombre distinguiendo mayúsculas** (ordenar `WindowsPath` las ignora), hora fija y **atributos Unix**
(`ZipInfo` usa MS-DOS por defecto en Windows). `.gitattributes` fija además `platforms/switch/test-mods/**` a LF.
El hash canónico cambia de `9c8a996c…` a `a6f964fe…` porque la compresión cambió; es el mismo en Linux y Windows.

`test-dusk-reproducible.sh` (funciona en Git Bash): clona las carpetas con `core.autocrlf` false/true/input y
`core.eol=crlf`, y además con los atributos de H36 (que sí dan ficheros CRLF): un único SHA-256 para todos, igual al
del árbol. En un paquete inventado comprueba que un binario con bytes CRLF no cambia, el orden, `stored`/Unix, y que
zlib-ng no cambia nada.

### 0.3 Limitación menor de Cosmetics: `COutFontSet_c::drawFont`

H36 encontró un único objetivo de hook sin llamada directa en el NRO: `COutFontSet_c::drawFont`. Cosmetics lo usa
(post-hook) solo para recolorear el icono de corazón insertado en el texto de los mensajes; si el juego nunca entra en
ese símbolo, ese corazón conserva su color. Los demás hooks de Cosmetics (anillo, botones, mapas, modelos de corazón…)
no dependen de él. Se acepta como limitación menor de V1; afecta igual al cargador dinámico (un hook se instala en la
entrada de la función, y ninguna vía intercepta una copia integrada en su llamador).

## 1. Estado

| pieza | estado | dónde se ha probado |
|---|---|---|
| Cargador ELF64 AArch64 (validación, Dry Run, mapeo, relocaciones) | hecho | host + qemu (código real ejecutado) |
| Política de importaciones Linux/Switch, shims | hecho | host |
| Manifiesto de símbolos post-link + allowlist de exportaciones | hecho | host, con enlaces AArch64 reales |
| Arenas de CodeMemory para funchook (bloqueo 0xCE01) | hecho | host + qemu (funchook real, 124 hooks) |
| Hooks entre mods: composición y conflictos | hecho | host (HookService real) |
| Modo seguro (boot guard) | hecho | host (caídas reales con `abort()`) |
| Integración Switch: Loader Mode, NativeModule, `linux-aarch64`, UI | hecho | sintaxis contra libnx; **sin ejecutar** |
| Navegador: comprobación antes de instalar, copia de seguridad | hecho | sintaxis; **sin ejecutar** |
| Randomizer V1 (parches, deps fijadas, hilos, blob) | hecho | compila; Dry Run y relocaciones en host |
| Randomizer funcionando en consola | **pendiente de H37-c1** | — |

## 2. Auditoría de los `.so`

Reproducida con `platforms/switch/tools/build-h37-test-mods.sh` (SDK `add_mod`, GCC 13.3 aarch64 de Ubuntu) y
`elf_audit.py`; datos completos en `docs/h37/audit.json`. Todos son ELF64 little-endian AArch64 `ET_DYN`, sin
`PT_INTERP`, con `PT_DYNAMIC`, `GNU_RELRO`, `GNU_EH_FRAME` y `.eh_frame`, `BIND_NOW`, `DT_GNU_HASH`.

| biblioteca | imagen | relocaciones (ABS64/GLOB_DAT/JUMP_SLOT/RELATIVE) | DT_NEEDED | init_array | importaciones del juego o del SDK |
|---|---|---|---|---|---|
| loader_lab | 131 KB | 5 / 8 / 22 / 25 | 4 | 3 | 0 |
| template_mod | 131 KB | 0 / 5 / 2 / 6 | 0 | 1 | 0 |
| rollandsprint | 132 KB | 4 / 7 / 16 / 9 | 2 | 1 | 18 |
| cosmetics | 332 KB | 31 / 14 / 66 / 104 | 5 | 5 | 37 |
| luau_runtime | 1.19 MB | 110 / 15 / 126 / 3088 | 5 | 29 | 0 |
| randomizer (parcheado) | 4.67 MB | 584 / 66 / 413 / 2212 | 5 | 33 | 656 |

Veredicto: **un `.so` Linux AArch64 no es directamente compatible**, y no se trata como tal. Es la misma
arquitectura y el mismo ABI de nombres de C++ (Itanium, libstdc++ `cxx11`) —**no** el mismo *layout* de los objetos que el compilador instancia en línea; ver §19—, pero se enlazó contra glibc: `DT_NEEDED` sobre
libc/libm/libstdc++/libgcc_s/ld-linux, versiones de símbolo (`GLIBC_2.17`, `GLIBCXX_3.4.x`…), nombres solo de glibc
(`__errno_location`, `__isoc23_*`, `__ctype_*_loc`…), objetos con tamaño de glibc (`pthread_mutex_t` 48 bytes frente a
12 de newlib). Se carga solo en **modo de compatibilidad** (§3.3), con cada diferencia tratada explícitamente. El
Randomizer publicado tiene además `PT_TLS` (un `thread_local`), que el cargador rechaza; el parche 1 (§9) lo quita.

## 3. Cargador ELF64 AArch64 (`src/dusk/mods/loader/elf_loader.cpp`)

Neutral de plataforma; tres etapas:

1. **`parse()`** — validación estricta con un código de error propio por fallo: truncado, clase/endianness/máquina/tipo
   incorrectos, cabeceras de programa fuera de rango, `PT_LOAD` fuera del fichero, desbordado o mal alineado,
   `filesz > memsz`, segmentos solapados o una página compartida con dos permisos, segmentos W+X, `PT_INTERP`,
   `PT_TLS`, `PT_GNU_RELRO` fuera de la imagen, `DT_TEXTREL`, RELR, REL, IFUNC/IRELATIVE, símbolos TLS, tablas de
   símbolos/cadenas/versiones malformadas, init arrays fuera de la imagen. Límites de tamaño (128 MiB fichero, 64 MiB
   imagen, 16 segmentos…).
2. **`plan()`** (Dry Run) — `DT_NEEDED` según la política del paquete, tipos de relocación, destinos de relocación en
   segmentos escribibles, importaciones resueltas **primero contra el propio módulo** (cada módulo usa sus propias
   copias de las plantillas del SDK) y después contra el juego, exports `mod_*`. Informe JSON
   (`dusklight-aleks-native-mod-report` v1). No reserva memoria ejecutable ni ejecuta nada.
3. **`map()`** — reserva, BSS a cero, copia, relocaciones, constructores reunidos (preinit, `DT_INIT`, init_array),
   exports, y permisos finales por página: RX código, R datos de solo lectura y RELRO, RW datos; **nunca W+X**.
   Los constructores solo se ejecutan con `run_constructors()`, después de todo lo anterior.

### 3.1 Relocaciones soportadas

`R_AARCH64_NONE`, `ABS64`, `GLOB_DAT`, `JUMP_SLOT`, `RELATIVE`: exactamente las que usan los seis módulos auditados.
Cualquier otra (TLSDESC, IRELATIVE, COPY, PREL…) se rechaza **por nombre**, sin implementaciones especulativas.

### 3.2 En Switch (`src/dusk/switch_native_loader.cpp`, `switch_elf_memory.cpp`)

- **Loader Mode** (Diagnóstico → *Native Mods: Loader Mode (H37)*, persistente): **Off** (no se lee nada),
  **Dry Run** (por defecto: valida y planifica contra este build, escribe
  `sdmc:/switch/dusklight/mods/.reports/<id>.json`, no ejecuta nada) y **Execute**. Un único NRO prueba cualquier
  paquete.
- Símbolos del juego: la propia `.dynsym` del NRO, leída en memoria a través de `_start` y la cabecera MOD0 de libnx
  (`image_symbols.cpp`, búsqueda por GNU hash o SysV hash). Solo contiene lo que el build exporta por nombre (§4).
- Memoria: el estilo de Horizon para un NRO — copia en heap, `svcMapProcessCodeMemory` en un rango libre de la región
  de código, `svcSetProcessMemoryPermission` por rango, invalidación de I-cache. No usa objetos CodeMemory.
- `.eh_frame` registrado con `__register_frame` (las excepciones C++ cruzan los marcos del módulo).
- **Constructores diferidos**: al descubrir un mod solo se valida y mapea; sus constructores se ejecutan al activarlo,
  dentro del boot guard. Un mod desactivado o en cuarentena nunca ejecuta código.
- `natives.cpp`: si el paquete no trae `lib/switch-arm64/mod.so`, usa `lib/linux-aarch64/mod.so`; el ABI del paquete
  (Switch o Linux) se lee del propio fichero (`DT_NEEDED`/versiones), no del nombre de la carpeta.
- **Descarga**: desactivar llama a `mod_shutdown`, retira callbacks y hooks y marca el mod inactivo; **el mapeo se
  conserva hasta salir del juego** (el código puede seguir en una pila o en una tabla de callbacks). No hay
  descarga real y no se afirma.

### 3.3 Dependencias y modo de compatibilidad Linux (`elf_runtime_policy.cpp`)

| caso | tratamiento |
|---|---|
| `DT_NEEDED` libc.so.6, libm.so.6, libstdc++.so.6, libgcc_s.so.1, ld-linux-aarch64.so.1 | el NRO los sustituye (solo paquetes Linux) |
| cualquier otro `DT_NEEDED` | rechazado por nombre (`needed_rejected`) |
| versiones de símbolo | se ignoran; el nombre debe existir en el NRO con el mismo significado |
| shims del cargador | `__cxa_finalize` (no-op: nunca se descarga), `__errno_location`, `__libc_single_threaded`, `__stack_chk_guard`, `stdin`/`stdout`/`stderr` |
| nombres solo de glibc que define el NRO | `switch_mod_libc.cpp` / `switch_mod_libstdcxx.cpp` (H36): `__ctype_*_loc`, `__isoc23_*`, `__printf_chk`, `getrandom`… |
| rechazados por nombre | `_IO_*`, `_dl_*`, `dl_iterate_phdr`, `__tls_get_addr`, `__cxa_thread_atexit_impl`, `__register_atfork`, `fork`, `syscall`, `__libc_*`, `gnu_get_libc_version` |
| con advertencia | `pthread_*`, `sem_*`: un `pthread_mutex_t` de glibc inicializado a cero es un mutex válido de newlib (tipo normal = 0); el inicializador **recursivo** estático de glibc no lo es (newlib lo trata como normal). Randomizer solo usa `std::mutex` y `std::thread` |
| importación débil ausente | resuelta a nulo |

Dependencias de Randomizer fijadas por commit (§9). Ninguna dependencia se descarga en tiempo de ejecución.

## 4. Manifiesto de símbolos y allowlist (`SymbolManifestSwitch.cmake`, `ModAbiExports.cmake`)

- `platforms/switch/mod_required_symbols.txt`, generado por `gen_required_symbols.py` a partir de los paquetes
  reales: 1159 importaciones y 18 hooks por nombre (Cosmetics 1, Randomizer 17, de ellos 12 estáticos de fichero, p. ej.
  `src/d/actor/d_a_npc_ks.cpp#action_check`).
- Las importaciones se añaden a una segunda `--dynamic-list` **nombre a nombre** (las de runtime C/C++ también como
  `--undefined`, para que el enlazador extraiga el miembro del archivo). **Sin `--export-dynamic`**: la `.dynsym` es
  la allowlist. Los símbolos fijados de H36 (`mod_abi_pinned.txt`), que antes se conservaban pero no se exportaban,
  ahora se exportan.
- Las 321 raíces `--undefined` van a GNU ld en un **fichero de respuesta** (`<build>/platforms/switch/
  mod_abi_undefined.rsp`, una `--undefined=NOMBRE` por línea, mismo conjunto y orden), pasado como un único
  `-Wl,@<fichero>`; ld lo lee él mismo. Como opciones sueltas sumaban ~14 500 caracteres y el enlace de H37-c1 en
  Windows falló en cmd.exe (*"La línea de comandos es demasiado larga"*, ~17 500 de 8191). El fichero solo se reescribe
  si cambia; el enlace depende de él, y editar las listas reconfigura. La ruta admite espacios, no comas (`-Wl` parte
  por comas; el configure lo rechaza con un mensaje).
- Tras cada enlace y antes de elf2nro: symgen (la versión 1.3.6 que ya fija `cmake/SymbolManifest.cmake`, con su
  SHA-256) genera el manifiesto de símbolos globales y locales en `romfs:/dusklight.symdb`, con el build-id GNU del ELF;
  `check_symbol_manifest.py` verifica que es de este enlace (build-id), que cada importación requerida está en la
  `.dynsym` y que cada hook por nombre resuelve a una sola dirección; escribe `romfs:/dusklight.symbols.json` (mods
  compatibles/incompatibles con los nombres que faltan) y, con `ALEKS2_SYMBOL_CHECK_STRICT=ON`, falla el build.
- H37-c2: la comprobación usa **la misma superficie que el cargador en consola** (`image_surface.py`): una importación
  vale si está en la `.dynsym` o, si no, en la **tabla de runtime de la imagen** (`aleks2_image_runtime`, §15.1), leída
  del propio ELF (símbolo y relocaciones `RELATIVE`) y verificada contra su `.symtab`: cada entrada debe ser el inicio de
  la función de ese nombre. Un hook por nombre, además de resolver a una sola dirección, tiene que ser **interceptable**:
  inicio de una función en código ejecutable, con sitio para el parche de dos instrucciones de funchook antes de la
  función siguiente (una función de una instrucción seguida de relleno NOP vale), sin saltos hacia el par parcheado, y
  alcanzada por algo (llamada o salto directo, también por veneer; puntero en datos; dirección formada con adrp+add;
  exportada). Un objetivo que nada alcanza se informa como tal, no como encontrado.
- symgen necesita DWARF: el build de Switch compila con `-g1` (`ALEKS2_SYMBOL_MANIFEST`, ON). `-g1` no cambia el
  código generado (comprobado: `.text` idéntico con y sin `-g1`); elf2nro descarta las secciones de depuración, así que
  el NRO no crece. Crecen el `.elf` y el tiempo de enlace.
- En consola, `manifest.cpp` toma el build-id de la cabecera NRO (elf2nro lo copia en `_start + 0x40`) y lee el
  manifiesto de la RomFS la primera vez que un nombre no está en la tabla de enlace de los mods integrados; lo rechaza
  si es de otro build.

## 5. Hooks entre mods (`svc/hook_conflicts.cpp`)

Los pre-hooks y post-hooks de cualquier número de mods se componen: cada uno con su contexto, por prioridad y orden de
registro. **Randomizer y Cosmetics hacen post-hook de `dMenu_Ring_c::_create` y se ejecutan juntos** (probado con el
`HookService` real). Los *replace* no pueden componerse: antes, un segundo mod con `HOOK_REPLACE_OVERRIDE` (o más
prioridad) quitaba el objetivo al primero **sin dejar rastro**. Ahora toda colisión entre dos mods se registra
(objetivo con su nombre, ambos ids, política, rechazada o desplazada) y se registra en el log; en Switch (V1) desplazar
está prohibido y se rechaza como conflicto, de modo que el titular sigue funcionando. El fallo del segundo mod se
categoriza como *hook conflict* en el modo seguro y el mensaje dice qué dos mods chocan y en qué función.

## 6. Arenas de CodeMemory para los trampolines (`platforms/switch/source/code_arena.c`)

### 6.1 Reglas del kernel que condicionan el diseño

| operación | regla |
|---|---|
| `svcCreateCodeMemory(addr, size)` | un objeto sobre memoria de heap alineada a página; cada objeto sale de un pool pequeño del kernel |
| `svcControlCodeMemory` MapOwner | vista **RW**, del objeto **entero** (tamaño = tamaño del objeto); una a la vez |
| MapSlave | vista **R/RX**, del objeto entero; una a la vez; **independiente** de la RW: pueden coexistir (así funciona `Jit` de libnx) |
| UnmapOwner / UnmapSlave | deshace cada vista por separado; un objeto mapeado no se libera al cerrar el handle |
| direcciones | ninguna de las dos vistas en las regiones heap/alias; la RX debe estar a ±4 GiB del objetivo (ADRP), la RW en cualquier sitio |

### 6.2 Diseño

- **Arenas de 64 páginas, un objeto CodeMemory cada una, mapeadas dos veces a la vez**: la vista **ejecutable**
  (MapSlave, R-X) junto al código enganchado y durante toda la vida de la arena; la vista **escribible** (MapOwner,
  RW-) en otra dirección, **solo mientras se escribe una página** (desde que se asigna hasta que se protege, o al
  reabrirla) y desmapeada en cuanto no queda ninguna página escribiéndose.
- **Una página por hook** (funchook pone una entrada por frontera de página en arm64). Página asignada: se escribe por
  la vista RW, *flush* de D-cache por la RW, se publica, invalidación de I-cache por la RX.
- **funchook recibe la dirección ejecutable** de su página: es la que su `adrp x9, page; br x9` debe alcanzar, la que
  debe devolver como trampolín original, y siempre es legible (funchook relee sus entradas tras proteger la página). Sus
  tres escrituras en página (cabecera en `get_page`, entrada en `prepare`, y el parche del objetivo calculado a partir de
  la dirección de la entrada) se redirigen a la vista RW mediante un parche en `cmake/PatchFunchook.cmake`
  (`funchook_arena_glue.c`). El configure de Switch vuelve a aplicar el parche (es idempotente) y se niega a compilar
  sin él, para que un árbol de build de H36 no compile un funchook antiguo con el backend nuevo.
- **Reutilización**: primero páginas nunca usadas; después la liberada hace más tiempo (un hilo que aún estuviera
  dentro de un trampolín recién retirado no ve su página reescrita). Las arenas se conservan toda la sesión.
- **Rollback**: si falla la creación, MapSlave o la reserva, no queda handle, reserva de virtmem ni memoria. Si falla
  MapOwner, la página sigue libre. Si falla UnmapOwner, se informa y se reintenta en el siguiente commit.
- **Sin title takeover**: el backend usa las syscalls 0x4B/0x4C que el lanzador actual ya concede (H36 creó 10
  objetos); el parche de las funciones del juego sigue en `code_patch_switch.c` (0x74/0x75), como en H36.

### 6.3 Objetos CodeMemory necesarios

| combinación | hooks (páginas) | objetos H36 | **objetos H37** |
|---|---|---|---|
| Cosmetics | 28 | 28 (falla en el 11.º) | **1** |
| Cosmetics + Roll and Sprint | 32 | 32 | **1** |
| + Randomizer | ≤ 124 (92 más; los objetivos compartidos usan una sola página) | 124 | **2** |

### 6.4 Carrera RW/RX

La vista ejecutable de una arena **no cambia nunca** mientras la arena existe: ni se desmapea ni cambia de permiso
al instalar o retirar hooks de otro mod. Escribir una página nueva solo mapea la vista RW (otra dirección); los
trampolines activos de otros mods, en cualquier hilo, siguen ejecutándose desde la RX intacta. No hay ningún mapeo W+X.
La única operación que desmapea la RX es `code_arena_trim()`, que solo libera arenas sin ninguna página en uso y que el
juego no llama.

### 6.5 Telemetría

Una línea por arena creada (dirección RX/RW, objetos en uso), una por fallo (operación y Result exacto), y un resumen
tras arrancar los mods y tras activar un mod en partida, también en Diagnóstico:
`code arenas: N CodeMemory object(s) (peak P, created C), pages T total / U used / F free, A allocated, R freed,
X reused, writable view mapped W time(s); last failure <op> 0x…`. Nunca una línea por fotograma.

## 7. Modo seguro (`src/dusk/mods/loader/boot_guard.cpp`)

- Alrededor de cada paso que ejecuta código de un mod por primera vez (carga: mapeo y relocación; constructores;
  `mod_initialize`) se escribe un marcador con el mod y el paso (escritura atómica) y se borra al volver.
- Un marcador presente al arrancar identifica el mod que tumbó el arranque anterior: queda en **cuarentena** (no se
  carga, con el motivo visible) hasta que el jugador lo apague y lo vuelva a encender (o lo recargue).
- Una caída de arranque sin marcador no se atribuye a ningún mod; **dos seguidas** y el siguiente arranque es **sin
  mods**; el siguiente vuelve a intentarlo. El jugador puede pedir un arranque sin mods (Diagnóstico → *Next Start
  Without Mods*, o el fichero `sdmc:/switch/dusklight/mods/.guard/start_without_mods`), válido para un solo arranque.
  Sin mods no borra nada: mods, su configuración, partidas y assets quedan como estaban.
- Categorías en `sdmc:/switch/dusklight/mods/.guard/report.json` (persistente, máximo 64 incidentes):
  `validation`, `missing_import`, `unsupported_relocation`, `missing_dependency`, `constructor_failure`,
  `initialize_failure`, `crash_during_init`, `hook_conflict`. El arranque se da por terminado 600 fotogramas
  después de iniciar los mods.

## 8. Navegador e instalación

El flujo existente se conserva: descarga a `.downloads/*.part`, tamaño y SHA-256 contra el catálogo, staging,
reemplazo atómico. Ninguna red nueva. En Switch con el cargador:

- Un paquete se instala **solo si su código nativo cargaría en este build**: Dry Run de su `lib/switch-arm64` (o
  `lib/linux-aarch64`) contra las exportaciones del NRO, sin mapear ni ejecutar nada, con informe. Si no, *"Not
  installed: …"* con el motivo; si solo trae binarios de otras plataformas: *"this mod's code is built only for
  windows-amd64, …; it has no switch-arm64 or linux-aarch64 build, so it cannot run on Switch"*. El navegador
  nunca muestra *instalado* para un mod que no puede arrancar.
- Una actualización guarda la versión anterior en `.backup/<id>.dusk`; si la nueva no carga, se restaura y se carga la
  anterior.
- Desinstalar solo borra el paquete: la configuración y `mod_data/<id>` (partidas y semillas de Randomizer) se
  conservan. Desactivar es el interruptor existente.
- Los paquetes de asset y Luau siguen igual (no tienen código nativo que comprobar).

## 9. Randomizer para V1

Serie de parches `platforms/switch/patches/randomizer/` aplicada sobre el commit fijado del submódulo (que no cambia)
cuando se construye el paquete:

1. **messages**: el único `thread_local` (buffer de texto formateado) pasa a `static`; el texto se resuelve solo en el
   hilo del juego. Sin él no hay `PT_TLS`.
2. **hilos y blob**: los hilos de generación de semilla y de precarga se conservan y se esperan (`join`) en el
   `shutdown` en lugar de `detach` (ninguno escribe en estado ya destruido); se registra el tiempo de generación
   (`seed generation succeeded in N ms`); la barra de progreso se actualiza desde `ui::update()` en el hilo del juego
   en vez de un hilo propio que llamaba al servicio de UI cada 16 ms; el blob `seed_hash` leído de una partida se
   limita (128 bytes) y no puede contener separadores, `..` ni caracteres de control antes de nombrar una carpeta de
   semillas — uno inválido se informa y la partida carga como normal.
3. **dependencias fijadas por commit**: yaml-cpp-0.9.0 `56e3bb550c91fd7005566f19c079cb7a503223cf` (MIT), base64pp
   v0.2.0-rc0 `736345896182b50babfe6eaa10b211b90d6abb0b` (MIT), TwilitRealm/embed twilit-v2
   `408567ae3eb296f3aa8d00e1c57d0b7d28ce992b` (Apache-2.0, incrustación en tiempo de build).
4. **(H37-c2) longitud del prefijo de `__FILE__`**: `SOURCE_PATH_SIZE` sale de `RANDOMIZER_SOURCE_PATH_PREFIX` (por
   defecto el directorio fuente) en vez de la ruta real; con `-ffile-prefix-map` el binario ya no depende del
   directorio de build ni `__FILENAME__` apunta fuera de `__FILE__` (§15.4).

Estado comprobado: el `.so` parcheado no tiene `PT_TLS`; el Dry Run lo acepta en modo de compatibilidad si el NRO
exporta sus 881 importaciones (656 del juego); sus 3275 relocaciones coinciden con readelf; 34 constructores
(`DT_INIT` y 33 de init_array); sus 17
hooks por nombre están en el manifiesto requerido; comparte con Cosmetics un post-hook que se compone.

El blob de partida vive en el sidecar JSON del host (`save.cpp`: versionado, escritura atómica, presupuesto de tamaño,
blobs inválidos descartados), separado de la partida del juego: no puede corromperla.

**No verificado (requiere consola)**: generación real de semillas (no se generan partidas en la nube), su tiempo y
memoria en Tegra, la pila de `std::thread` en libnx durante la generación, y el juego con una semilla.

## 10. Escalera de compatibilidad

| nivel | paquete | qué demuestra | host |
|---|---|---|---|
| 0 | `loader_lab` | constructores, objeto estático, libstdc++, vtables, excepción C++, 3 ciclos init/update/shutdown | ejecutado bajo qemu |
| 1 | Roll and Sprint dinámico | 18 símbolos del juego, 4 hooks | Dry Run + relocaciones |
| 2 | Cosmetics | 37 símbolos del juego, 28 hooks, servicios UI/textura/config | Dry Run + relocaciones |
| 3 | Randomizer | 656 símbolos del juego, 92 hooks (17 por nombre), hilos, blob | Dry Run + relocaciones |
| 4 | ALBW / Dawnlight | — | **no auditados**: el catálogo (twilitrealm.dev) devuelve 403 desde esta red y sus fuentes no están disponibles |

`luau_runtime` además se ejecutó completo bajo qemu cargado por el cargador, ejecutando un script.

## 11. Pruebas (host)

| script | qué cubre |
|---|---|
| `test-elf-loader.sh` | 6 bibliotecas reales: cada relocación contra readelf, permisos, constructores, exports; 36 corrupciones dirigidas, cada una con su código; 12000 mutaciones deterministas sin fallo (encontró el cuelgue de `PT_GNU_RELRO`); ASan+UBSan+`_GLIBCXX_ASSERTIONS`; ejecución real bajo qemu (lab, template, Luau runtime con script); el veredicto de `elf_audit.py` coincide con el Dry Run en las 12 combinaciones biblioteca×ABI |
| `test-symbol-manifest.sh` | juegos AArch64 enlazados como el NRO (GNU/SysV/ambos hash, 3000 exportaciones), symgen fijado, comprobador (compatible, no exportado, indefinido, estático ambiguo, hook ausente, manifiesto obsoleto, comprimido), TLS exportado nunca resuelto, `mod_required_symbols.txt` regenerado igual |
| `test-code-arena.sh` | A: kernel simulado que rechaza el 11.º CodeMemory (vistas memfd reales): combinaciones de mods, rollback tras fallos de create/MapSlave/MapOwner/UnmapOwner, fuera de alcance, orden de reutilización, Cosmetics apagado/encendido sin crecer, sin fugas. B: funchook v1.1.3 con el parche del build, 124 hooks reales bajo qemu; sin el parche o sin el ajuste de `new_code`, cae |
| `test-hook-conflicts.sh` | `HookService` real: composición en `dMenu_Ring_c::_create`, conflictos con desplazamiento permitido y prohibido; contra el `hook.cpp` de H36 fallan 10 comprobaciones |
| `test-boot-guard.sh` | caídas reales (`abort()` en procesos hijo): cuarentena por paso, arranque sin mods tras dos caídas sin marcador, petición del jugador, informe acotado, ficheros dañados, categorías |
| `test-dusk-reproducible.sh` | §0.2 |
| `test-link-response-file.sh` | proyecto CMake+Ninja real en rutas con espacios que incluye `ModAbiExports.cmake`: sin `--undefined` en la orden de enlace (≈1 700 caracteres), raíces idénticas a las listas y en su orden, raíces C y C++ extraídas del archivo y exportadas, sin reenlace si nada cambia, reconfiguración + reenlace al editar la lista, comprobación post-link sobre la imagen; con el fichero anterior falla |
| `test-elf-loader.sh` (H37-c2, etapa 5) | imagen static-pie AArch64 enlazada como el NRO (libgcc_eh oculta): sin la tabla de runtime `_Unwind_Resume` falta (el Dry Run de H37-c1); con ella es el `_Unwind_Resume` de la propia imagen; un módulo ejecuta su constructor una vez y las excepciones cruzan sus frames en los tres sentidos (1004 limpiezas); sin `__register_frame` el mismo módulo acaba en `std::terminate`; las importaciones ocultas de loader_lab (1), Cosmetics (7) y Randomizer (9) resuelven por la tabla |
| `test-hook-targets.sh` | las 15 unidades del juego que definen los 18 objetivos de hook de los paquetes oficiales, compiladas con las opciones de Switch leídas de los CMake, y otra vez sin inlining: cada objetivo existe con el nombre de symgen, es interceptable, el DWARF no tiene copias inlineadas y no se pierde ninguna llamada directa; control: lo inlineado se rechaza, lo `noinline` se acepta y `-g2` genera el mismo código que `-g1`; sin el arreglo falla en exactamente los 6 objetivos |
| `test-h37-candidate.sh` | `check_h37_candidate.py` con los cuatro paquetes reales y un juego sustituto enlazado como el NRO que les da exactamente lo que piden: READY; NOT READY con su motivo al faltar una importación, un hook sin llamadas, uno inlineado, la tabla de runtime, el NRO o el manifiesto de este enlace, un paquete, o con el árbol sucio |
| `test-indirect-references.sh` (H37-c3) | static-pie AArch64 real (clang + ld.lld), con RELA y con RELR: callback en un `FuncTable` local, en tabla de métodos ← perfil ← lista de perfiles ← código, ABS64 reescrito; control con tabla muerta (y método+perfil muertos) y con addend `función+4`, ambos rechazados; relocación con lugar fuera de la imagen ignorada |
| `test-h37-mods-reproducible.sh` | `build-h37-test-mods.sh` en dos directorios distintos: los `.so` y los `.dusk` idénticos byte a byte; cada `.dusk` canónico |
| `test-luau-probe.sh`, `test-static-mods.sh` y el resto | las pruebas de H28–H36, sin regresiones |

## 12. H37-c1

### 12.1 Build

```
cmake --preset switch-devkitA64-release-h37 -DALEKS2_BUILD_TAG=H37-c1
cmake --build --preset switch-devkitA64-release-h37
```

Equivale a `switch-devkitA64-release` con `ALEKS2_NATIVE_MOD_LOADER=ON`, `ALEKS2_STATIC_MODS=luau_runtime` y
`ALEKS2_SYMBOL_CHECK_STRICT=OFF`. El configure descarga symgen 1.3.6 (con SHA-256) si `SYMGEN_PATH` no apunta a uno.
Usar un **directorio de build nuevo** o al menos reconfigurar: el configure vuelve a parchear funchook. En el log de
build debe aparecer `symbol manifest: build …` y una línea por mod (`compatible` o `INCOMPATIBLE` con los nombres).

### 12.2 Ficheros de la SD

- `sdmc:/switch/dusklight/dusklight.nro` (H37-c1).
- `sdmc:/switch/dusklight/mods/`: la carpeta `luau_runtime/` de `switch-mods/` del build; **quitar** las carpetas
  `cosmetics/` y `rollandsprint/` de H36 (ahora vienen como `.dusk`).
- Paquetes, por niveles (§12.3): `loader_lab.dusk`, `rollandsprint.dusk`, `cosmetics.dusk`, `randomizer.dusk`, hechos
  con `platforms/switch/tools/build-h37-test-mods.sh OUT` (Linux/WSL con `aarch64-linux-gnu-g++`; aplica los parches de
  Randomizer). Un paquete publicado distinto puede importar otros nombres: el Dry Run lo dirá.

### 12.3 Protocolo mínimo, en orden

1. **Nivel 0, Dry Run.** Solo `loader_lab.dusk`. Arrancar. En el log: `game image exports N symbol(s)`, `native mod
   loader: Dry Run (boot 1)`; en `mods/.reports/dev.aleks2.loader_lab.json` `"ok":true`; el mod aparece rechazado con
   *compatible …; not run because Loader Mode is Dry Run*.
2. **Execute.** Diagnóstico → Loader Mode → Execute; reiniciar. Log: `mapped at …`, las líneas `[h37-lab]` (constructores,
   excepción capturada, `update 100`…).
3. **Nivel 1.** Añadir `rollandsprint.dusk`, reiniciar: rodar/correr funcionan; línea `code arenas: 1 CodeMemory
   object(s)`.
4. **Nivel 2.** Añadir `cosmetics.dusk`, reiniciar: **sin `0xce01`**; los 28 hooks; colores visibles; `code arenas: 1
   CodeMemory object(s) … pages 64 total / 32 used`. Apagar y encender Cosmetics en partida dos veces: Roll and Sprint
   sigue funcionando; el número de objetos no crece.
5. **Nivel 3.** Añadir `randomizer.dusk`, reiniciar: `code arenas: 2 CodeMemory object(s)` como máximo; generar una
   semilla en consola (anotar `seed generation succeeded in N ms` y si el juego responde durante la generación); empezar
   una partida Randomizer, guardar, salir, recargar.
6. **Modo seguro.** Diagnóstico → Next Start Without Mods; reiniciar: ningún mod cargado, todo intacto; el siguiente
   arranque vuelve a cargarlos.
7. Recoger: log de sesión, `mods/.reports/*.json`, `mods/.guard/report.json`, `dusklight.symbols.json` del build.

## 13. Riesgos que solo pueden comprobarse en Switch

- Que MapOwner y MapSlave de un mismo objeto coexistan como en libnx `Jit`, y que MapSlave antes que MapOwner sea
  aceptado (libnx los mapea en el orden contrario; el kernel los trata como independientes).
- Que el pool de objetos CodeMemory tenga al menos 2 libres para el juego (H36 llegó a 10).
- `svcMapProcessCodeMemory`/`SetProcessMemoryPermission` para los módulos (mismas syscalls que funchook ya usaba en
  H35/H36 para su comprobación inicial; nunca se han usado para mapear un módulo).
- Que el desenrollador de libgcc de devkitA64 acepte `__register_frame` de un módulo cargado (en host, con la libgcc_eh
  oculta de una imagen estática, sí: §15.1).
- Compatibilidad real glibc→newlib de las funciones importadas por Cosmetics/Randomizer (semántica, no solo nombres).
- Generación de semillas de Randomizer: tiempo, memoria y pila de `std::thread` de libnx.
- Que el post-hook de `COutFontSet_c::drawFont` nunca se dispare (limitación aceptada).

## 14. Bloqueos restantes

- Ninguno conocido en host. Todo lo que falta es verificación en consola (§13) y los niveles 4 (ALBW/Dawnlight), sin
  paquetes ni fuentes accesibles desde esta red.

## 15. H37-c2: auditoría del ELF final de H37-c1

H37-c1 (`c0ebdbe2`) compiló en local y su NRO/ELF verificaron (manifiesto no obsoleto, 3383 exportaciones), pero la
auditoría contra el artefacto final dejó tres paquetes sin poder pasar el Dry Run: loader_lab (`_Unwind_Resume`),
Cosmetics (7 funciones de libgcc) y Randomizer (esas 7, `sqrt`, `sqrtf` y 5 objetivos de hook). Roll and Sprint, 19/19.
H37-c1 queda como diagnóstico; no va a la Switch.

### 15.1 Funciones de runtime ocultas: la tabla de runtime de la imagen

**Causa, comprobada en ambos lados.** Estas funciones están en el ELF pero con visibilidad `hidden`:
- en `libgcc_eh.a`: `_Unwind_Resume`;
- en `libgcc.a`: `__eqtf2`, `__gttf2`, `__netf2`, `__unordtf2`, `__trunctfdf2` (comparación y conversión de
  `long double`, que en AArch64 es IEEE binary128) y `__udivti3` (división de 128 bits);
- en la libm de la imagen de H37-c1: `sqrt` y `sqrtf`.

El enlazador convierte una definición oculta en símbolo local (`nm` muestra `t`). La deja fuera de `.dynsym` diga lo
que diga `--dynamic-list`, y `ImageSymbols` solo lee `.dynsym`. La libgcc de la toolchain aarch64 de Linux declara
exactamente igual esas siete funciones y `__register_frame`, lo que permite reproducir el caso en host.

**Solución:** `src/dusk/mods/loader/elf_image_runtime.c`, una tabla explícita, en C, con nombre de importación →
dirección de la propia función de la imagen. La dirección la resuelve el enlazador para una referencia desde dentro de
la imagen, como ya hacía `switch_native_loader.cpp` con `__register_frame`. `make_resolver` la consulta después de las
exportaciones, para cualquier ABI de paquete.

| símbolo | biblioteca | qué es | resuelto a |
|---|---|---|---|
| `_Unwind_Resume` | libgcc_eh | fin de los *landing pads* de limpieza | el desenrollador de la imagen, el mismo en el que `__register_frame` registra el `.eh_frame` del módulo |
| `__eqtf2`, `__netf2`, `__gttf2`, `__unordtf2` | libgcc | comparaciones binary128 | las funciones de la imagen |
| `__trunctfdf2` | libgcc | binary128 → double | ídem |
| `__udivti3` | libgcc | división de 128 bits | ídem |
| `sqrt`, `sqrtf` | libm | la función de biblioteca (no el `fsqrt` que el compilador inlinea en sus propias llamadas) | ídem |

Alternativas descartadas:
- **Exportar locales desde el manifiesto:** symgen solo da nombre y dirección, sin tipo, y con nombres que pueden ser
  ambiguos. La tabla, en cambio, la resuelve el enlazador mismo.
- **Envoltorios con el mismo nombre:** imposibles, porque chocan con la definición oculta de libgcc.
- **Enlazar libgcc dentro del mod:** valdría para las funciones sin estado, pero no para `_Unwind_Resume`. Una segunda
  libgcc_eh tendría su propio registro de frames, vacío.

Propiedades:
- **Lista cerrada:** nueve nombres; nada se exporta en bloque ni se busca un local por nombre en tiempo de ejecución.
- **ASLR:** las direcciones son punteros en `.data.rel.ro` con relocaciones `RELATIVE`, que crt0 reubica con el resto
  de la imagen.
- **Verificada tras cada enlace:** `check_symbol_manifest.py --require-image-runtime` lee la tabla del ELF y comprueba
  cada entrada contra la `.symtab`.

Prueba (`test-elf-loader.sh`, etapa 5, qemu), con una imagen static-pie enlazada como el NRO:
- sin la tabla, `_Unwind_Resume` falta: el mismo resultado que el Dry Run de H37-c1;
- con ella, la importación es el `_Unwind_Resume` local de la imagen;
- el constructor del módulo se ejecuta una sola vez;
- las excepciones atraviesan frames del módulo imagen→módulo→imagen, dentro del módulo y módulo→imagen, 1004
  limpiezas;
- control: sin `__register_frame`, `std::terminate`;
- con los paquetes reales, las importaciones ocultas de loader_lab (1), Cosmetics (7) y Randomizer (9) pasan por la
  tabla.

### 15.2 Los cinco objetivos de hook de Randomizer (y un sexto)

**Causa común.** Las cinco funciones existen en el código fuente con su nombre; no había otro nombre, *mangling* ni
alias. Compiladas con las opciones de Switch (-O3, sin clones IPA, `-g1`), su código quedaba inlineado en los
llamadores: sin copia propia, o con una copia sin llamadas que `--gc-sections` eliminaba.

No se ha fabricado ningún stub, porque un hook necesita que el código que el juego ejecuta pase por la entrada de la
función. Las seis llevan ahora `DUSK_NOINLINE` en su definición, el atributo que el juego ya usa para funciones
enganchables.

| objetivo | fichero | causa | después |
|---|---|---|---|
| `b_bq_end` | `d_a_b_bq.cpp` | `static`, un llamador, inlineada | 240 B, 1 llamada |
| `daWindStone_c::chkEveOccur` | `d_a_obj_wind_stone.cpp` | miembro pequeño, dos llamadores, inlineada en ambos | 40 B, 3 llamadas |
| `lure_heart` | `d_a_mg_rod.cpp` | `static`, un llamador | 968 B, 1 |
| `src/d/actor/d_a_npc_ks.cpp#action_check` | `d_a_npc_ks.cpp` | `static`, un llamador | 3080 B, 1 |
| `uki_catch` | `d_a_mg_rod.cpp` | `static`, un llamador | 2380 B, 1 |
| `dComIfGp_setNextStage` (completa) | `d_com_inf_game.cpp` | sí estaba, pero inlineada en la sobrecarga de 4 argumentos y en `dComIfGs_gameStart` | 608 B, 3 |

El sexto, `dComIfGp_setNextStage`, aparece en el manifiesto, pero el hook no veía los cambios de escenario hechos a
través de esas dos funciones. Lo encontró la misma medición.

Sobre `action_check`: el nombre sin ruta es ambiguo porque hay otro `action_check` en otro fichero. symgen normaliza
`\` → `/` y compara la unidad sin distinguir mayúsculas, así que el nombre con ruta vale también en Windows.

**Coste en `.text`:** +120 B (`d_a_b_bq`), +216 (`d_a_mg_rod`), +272 (`d_a_npc_ks`), +100 (`d_a_obj_wind_stone`), −828
(`d_com_inf_game`).

**`daE_MD_Create`** mide 4 bytes: es un único `b` de llamada final. Es interceptable porque el relleno de alineación
(NOPs) deja sitio a las dos instrucciones de funchook, que reubica el `b`. La comprobación ya no exige 8 bytes de
función, sino 8 bytes hasta la siguiente.

**Prueba (`test-hook-targets.sh` + `hook_inline_audit.py`).** Compila las 15 unidades del juego que definen los 18
objetivos de hook de los paquetes oficiales. Usa las opciones leídas de `cmake/SwitchOptions.cmake` y `CMakeLists.txt`
y compila cada unidad dos veces, con inlining y sin él. Para cada objetivo exige:
- symgen lo nombra igual que el manifiesto post-link;
- es interceptable;
- el DWARF (a `-g2`, que genera el mismo código que `-g1`) no tiene ninguna copia inlineada;
- no se pierde ninguna llamada directa respecto a la compilación sin inlining.

Una unidad de control verifica la propia prueba: lo inlineado se rechaza y lo `noinline` se acepta. Sobre el código
anterior falla exactamente en los 6 objetivos; ahora pasa 18/18.

### 15.3 Comprobación estricta de candidata

El preset H37 sigue con `ALEKS2_SYMBOL_CHECK_STRICT=OFF` para desarrollo. Antes de entregar una candidata se construye
el target `dusklight_h37_candidate` (`check_h37_candidate.py`), que nunca es indulgente. Escribe
`<build>/h37-candidate.json` y falla salvo que se cumpla todo esto:
- el NRO lleva el build-id del ELF;
- el manifiesto es de este enlace;
- las fuentes son un commit limpio (dirty=0);
- los cuatro paquetes oficiales, leídos de los `.dusk` mismos y no de la lista versionada, resuelven cada importación
  (`.dynsym` o tabla de runtime) y cada hook (manifiesto, sin ambigüedad, interceptable) contra el ELF, como lo hará el
  cargador.

`test-h37-candidate.sh` lo ejercita con los paquetes reales: un juego sustituto con todo lo que piden da READY, y cada
fallo plantado da NOT READY con su motivo.

### 15.4 Paquetes reproducibles

En H37-c1 había dos fuentes de variación:
- los `.dusk` del SDK llevaban la hora del zip;
- los `.so` llevaban el directorio de build en el DWARF (`.debug_line_str`) y, a través de él, en el build-id GNU.

`build-h37-test-mods.sh` ahora:
- compila con `-ffile-prefix-map`: OUT_DIR → `/h37-mods`, el repositorio → `/dusklight`, las fuentes externas →
  `/deps/<nombre>`;
- entrega como resultado principal el `.dusk` canónico de `make_dusk.py`, rehecho a partir del paquete del SDK, y el
  `mod.so` extraído de él;
- escribe `SHA256SUMS`.

El Randomizer necesitaba algo más. Su `__FILENAME__` es `&__FILE__[SOURCE_PATH_SIZE]`, con la longitud de la ruta
real; con el mapeo, el desplazamiento cambiaba con el directorio y podía quedar fuera de la cadena. El parche
`0004-source-path-size-from-the-file-prefix.patch` lo calcula a partir del prefijo mapeado
(`RANDOMIZER_SOURCE_PATH_PREFIX`; sin mapeo, todo igual).

`test-h37-mods-reproducible.sh` construye dos veces, en directorios de nombre y longitud distintos, y exige:
- los mismos bytes en cada `.so` y cada `.dusk`;
- ninguna ruta de build dentro de los `.so`;
- cada `.dusk` canónico, y que al reempaquetarlo salgan los mismos bytes.

Resultado en la nube: los mismos bytes desde `…/out/` y desde `…/a-longer-second-output-directory/`. Los valores dependen
de la toolchain (Ubuntu `aarch64-linux-gnu-g++` 13.3); otra versión de GCC da otros valores, pero igual de estables
entre rutas.

| paquete | `mod.so` (SHA-256) | `.dusk` (SHA-256) |
|---|---|---|
| loader_lab | `ba3f005f84fb340431e972f48e5e0f103a664e15f0d836628dfc37e27bf06e06` | `096a217d4d6f58290a477e2391d9c75f6b6f089e6c156c2b3aa3b2d919c3eccf` |
| rollandsprint | `c3ee185ba9c426f9a7fad7cedbfa18ef2ec211a07296d6ce6e891394d86cdda1` | `c397ed86cd52534593e796ce156bc399fb5876c75e2250174dd9e2a3c1007cbd` |
| cosmetics | `2a02e249e267eaeb4f023a09cfb67d1e87eac3c40d2c0d90c3c585f40c1399d0` | `d14b39588a52e7af5124762341b25ad376456ca1f5d86aaf297ca596ba73e3e7` |
| randomizer | `3d4c4655908122a48fd52b227faf0bc007cf270b045093bfd00de72fd829b2e2` | `186a81b124c13f9eba174a3eb2480d96e61e6865127747b5063294a401d28152` |
| luau_runtime | `ec86c9d947c0767704ef81712b392d8bc638aefc6bca88bd0582eb5a48894d90` | `d15bac2abacd12119984d635831433d58b13d089725d2d45cf7df4c6daf5f054` |
| template_mod | `4ba5f7da8e925b44938fb43c668fa3a2ceefe52f7d107d6e2aeec191f71c595c` | `1bf8690fdc74a18bbd09f49d3e48a05df0fbed454caf85b0af9dd3fb0ae0bca7` |

### 15.5 `test-dusk-reproducible.sh` en Git for Windows

Fallaba con *"no CRLF files"* por dos motivos:
- el `grep` de Git for Windows descarta el CR de una línea CRLF antes de comparar (salvo con `-U`), así que
  `grep $'\r'` no encontraba nada;
- el control dependía de que Git convirtiera a CRLF.

Ahora:
- toda comprobación de bytes la hace Python sobre los bytes;
- el control es una copia de cada paquete con sus ficheros de texto reescritos a CRLF aquí mismo, y se comprueba que lo
  son;
- la salida de Python se limpia de CR.

Simulado con un `grep` que nunca encuentra CR y un `python3` que escribe CRLF: la versión anterior falla con
*"no CRLF files"* y la nueva pasa.

### 15.6 Los dos avisos nuevos

- **`loader.cpp:1225`:** se ignoraba el resultado `[[nodiscard]]` de `reload_runtime_mod` al restaurar la versión
  anterior tras una actualización que no carga. Esa recarga puede fallar, y el mensaje decía "restored" en cualquier
  caso, incluso si el paquete restaurado no se podía leer. Ahora cada caso tiene su mensaje.
- **`POINTER_ADD` redefinido:** `ImGuiMenuTools.cpp` incluye `aurora/lib/internal.hpp` sobre el PCH, que ya trae
  `global.h`, y las dos cabeceras definen el macro (con `TYPEOF` y con `decltype`, iguales en C++). Ese fichero ahora
  retira las del PCH antes de incluir las de Aurora; el submódulo no cambia.

Los dos avisos se reproducen con AArch64 GCC y las opciones de H37, y desaparecen.

### 15.7 Build local de H37-c2

```
git fetch origin claude/h37-dynamic-mod-loader
git checkout claude/h37-dynamic-mod-loader
git merge --ff-only origin/claude/h37-dynamic-mod-loader
cmake --preset switch-devkitA64-release-h37 -DALEKS2_BUILD_TAG=H37-c2 -DALEKS2_H37_PACKAGES_DIR=<carpeta de los .dusk>
cmake --build --preset switch-devkitA64-release-h37
# paquetes (Linux/WSL, con aarch64-linux-gnu-g++):
platforms/switch/tools/build-h37-test-mods.sh <carpeta de los .dusk>
# antes de entregar:
cmake --build --preset switch-devkitA64-release-h37 --target dusklight_h37_candidate
```

El build es incremental. Ninguna cabecera compartida cambia: se recompilan las seis unidades de los hooks
(`d_a_b_bq`, `d_a_mg_rod`, `d_a_npc_ks`, `d_a_obj_wind_stone`, `d_com_inf_game`, más `ImGuiMenuTools`), `loader.cpp`,
`elf_runtime_policy.cpp` y la nueva `elf_image_runtime.c`; luego se vuelve a configurar (cambian `CMakeLists.txt` y
`SymbolManifestSwitch.cmake`) y a enlazar.

En el log del post-link:
- `… exported, 9 in the image runtime table`;
- las cuatro líneas de mods `compatible`.

En `h37-candidate.json`: `"ready": true`.

## 16. H37-c3: referencias indirectas en el auditor

El gate de H37-c2 dio HOOKS 12/17: `dStage_playerInit`, `daDitem_Execute`, `daE_MD_Create`, `daKytag08_Execute` y
`daObj_Gb_Create` salían con 0 llamadas, 0 `data_pointers` y 0 `formed_in_code`. Son callbacks de tablas
(`FuncTable` local, `l_*_Method` ← `g_profile_*`), nunca el operando de un `BL`. Los símbolos y su espacio para
funchook eran correctos; fallaba la clasificación de sus referencias. Reproducido en un static-pie AArch64 real:

- con `DT_RELR` (relocaciones empaquetadas) el auditor anterior daba 0 punteros para todos los callbacks de tabla, que
  es exactamente lo observado; solo leía `SHT_RELA`/`R_AARCH64_RELATIVE`;
- el enlazador relaja `adrp+add` en un único `adr`, que el auditor no veía como dirección formada en código;
- aceptaba cualquier puntero, aunque la tabla estuviera muerta (un `cb_dead` en una tabla sin uso pasaba).

`image_surface.py` ahora:
- lee las relocaciones como las aplica el cargador: `DT_RELA`/`DT_RELR` (y las secciones asignadas):
  `RELATIVE` (destino en el addend), `ABS64`/`GLOB_DAT` (símbolo definido + addend) y RELR; el lugar debe estar
  dentro de un segmento cargado y alineado a 8;
- un puntero cuenta solo si la tabla que lo contiene (símbolo `OBJECT`, o la ranura de `.got`) está en uso: exportada,
  su dirección se forma en código (`adrp`+`add`/`ldr`/`ldp`, o `adr`) o la apunta una tabla en uso (método → perfil →
  lista de perfiles → código); ciclos y tablas sin uso son muertas;
- el destino debe ser el inicio exacto de la función; `función+4` no cuenta;
- el espacio para funchook se comprueba igual que antes. No se añade `DUSK_NOINLINE`.

El informe de cada hook trae `data_pointers` (vivos), `dead_pointers` e `indirect_references` (sección, tipo de
relocación, valor, tabla, cadena y cómo se llega a ella).

El ELF auditado de H37-c2 no estaba disponible en esta sesión: el tipo exacto de relocación de los cinco queda por
confirmar con `readelf -r` sobre él; el auditor acepta RELATIVE, ABS64, GLOB_DAT y RELR.

### 16.1 Paquetes locales exactos

La reproducción exacta de los paquetes locales de H37-c2 usó `FMT_SRC` (fmt 12.1.0) y `COSMETICS_SRC` (mapeado a
`/deps/cosmetics` por `-ffile-prefix-map`), como describe la cabecera de `build-h37-test-mods.sh`.

## 17. H37-c4: el servicio de archivos en Switch y la validación de servicios

### 17.1 Qué falló en consola (H37-c3)

H37-c3 en hardware: `loader_lab`, Roll and Sprint y Cosmetics (28 hooks) cargaron en Execute con un solo objeto
CodeMemory y 0 errores; el modo seguro funcionó. Randomizer falló al activarse:
`Mod requires a service that is unavailable (missing: dev.twilitrealm.dusklight.file)`. Causa:
`file_available()` solo miraba si había un selector de archivos, y `borealis_file_select_switch.cpp` no ofrece ninguno
(`capabilities()` vacío), así que el servicio no se registraba en Switch. Randomizer lo importa por la mitad que no
necesita selector: leer y escribir por ubicación (`borealis::io`), que sí funciona en la tarjeta. El Dry Run y el gate
de c3 decían "compatible" porque nunca miraban los servicios, solo imports y hooks.

### 17.2 Cambios

- `svc/file_availability.hpp`: `file_service_available()`. En Switch (`__SWITCH__`) el servicio existe sin selector.
  `file.cpp` lo usa en `file_available()`; en escritorio no cambia nada.
- `pick_file`, `pick_folder` y `export_file`, sin selector, ya no llegan a un stub que responde dentro de la llamada:
  la petición termina como `MOD_UNAVAILABLE` ("canceled") en el frame siguiente, como un diálogo cerrado en escritorio.
  Las funciones por ubicación (check, open, size, read, write, seek, flush, close, read_all, write_all, free, list, join,
  create_child) no se tocan.
- `mods/loader/mod_services.{hpp,cpp}`: lee los `ModMetaImport` de la sección `modmeta` de la biblioteca (datos, no hace
  falta mapear ni ejecutar) y juzga los servicios `dev.twilitrealm.dusklight.*` contra la build: obligatorio ausente,
  minor o major demasiado nuevo, o mod hecho para un major anterior bloquean, con el texto de
  `ModLoader::resolve_service_imports`; un servicio opcional ausente se lista y no bloquea; los servicios de otros mods
  se dejan al cargador. `svc::host_modules()` es la única lista de servicios del juego (`init_services` la registra y la
  comprobación la lee). `switch_native_loader.cpp` la aplica en Dry Run, en Execute y en la comprobación de instalación;
  el informe de Dry Run gana un arreglo `"services"` y pasa a `"ok":false` si uno bloquea.
- Gate (`check_h37_candidate.py`): `elf_audit.select_library()` toma del paquete la misma biblioteca que el cargador
  (`switch-arm64` y, si no, `linux-aarch64`; las demás plataformas se ignoran), en lugar de exigir una sola; los
  servicios de cada paquete se leen con `symgen modmeta` y se juzgan con `host_services.py` (versiones de las
  cabeceras del SDK; lista explícita de lo que Switch no ofrece: `net` y `websocket`). Un paquete con un servicio
  obligatorio imposible ya no puede salir "compatible". `--sdk-include` permite indicar las cabeceras.

### 17.3 Pruebas

| prueba | dónde | qué cubre |
|---|---|---|
| `test-mod-services.sh` | host (g++, ASan/UBSan) | servicio de archivos disponible sin selector; lectura de `modmeta` de un ELF sintético y todas sus formas malformadas; obligatorio/opcional/versiones/servicio ajeno; informe anotado; con `H37_RANDOMIZER_SO`, los 15 servicios reales de Randomizer satisfechos (control negativo con `file` no disponible: reproduce el mensaje de c3) |
| `test-host-services.sh` | host (Python) | selección de `linux-aarch64` en un paquete multiplataforma (y `switch-arm64` primero en cualquier orden, error con las plataformas presentes); servicios del gate con las mismas reglas; la lista "no disponible" coincide con las fuentes; Randomizer real satisface sus servicios |
| `test-switch-io.sh` | Linux/WSL (g++, ASan/UBSan, SDL de pruebas sobre stdio) | `borealis_io_switch.cpp`: check, open (lectura, truncar, añadir), size, read, write, seek, flush, close, `read_all`/`write_all` compuestos como el servicio, list, join, create_child, display_name, atomic_replace y sus rechazos; el selector: capacidades vacías y los tres métodos responden "canceled" |
| `test-h37-candidate.sh` | Linux (con symgen) | el gate sobre los cuatro paquetes reales: READY con servicios 1/1, 6/6, 4/4 y 15/15 |

Límite: las funciones del servicio en `file.cpp` (open/read/write... que envuelven `borealis::io`) solo se compilan en la
build de Switch; en host se ejecuta la capa de plataforma que usan, no su envoltorio.

### 17.4 Mods del catálogo, resultado actual (sin implementar su compatibilidad)

Auditados en el PC con el ELF de H37-c3 (`for-cloud-mod-audit/AUDIT-SUMMARY.txt`); los tres traen bibliotecas para seis
plataformas y se evaluó `linux-aarch64`:

| mod | imports que faltan | hooks por nombre | servicios |
|---|---|---|---|
| Twilit Essentials 2.4.2 | 33 de 920 | 16 de 20 (faltan 4) | `file` opcional |
| ALBW 0.3.0 | 11 de 987 | 41 de 66 (faltan 25) | todos |
| Dawnlight 3.8.1 | 31 de 1049 | 31 de 36 (faltan 5) | `com.kite.kh2hud.drive` opcional (de otro mod) |

(Actualizado en §18: además, Twilit Essentials y Dawnlight traen `PT_TLS`, que el cargador rechaza.) Todos serían rechazados por el cargador por símbolos del juego que la imagen no exporta (la lista de exportación sale
solo de los cuatro mods ya auditados) y por hooks de funciones sin símbolo propio. H37-c4 no cambia esto.

## 18. H37-c5: rutas con montaje, fallos en ejecución y requisitos del catálogo

Base: H37-c4 `2d30b7a3cbbf6efc414ee54f1f931f8015fbb98a`. Evidencia: `H37-C5-CLOUD-EVIDENCE.zip` (dos logs del crash de
Randomizer en consola, `guard-report.json`, la auditoría de los tres mods contra el ELF de H37-c3). Los gitlinks de
Aurora, Borealis, Cosmetics y Randomizer no se mueven.

### 18.1 La ruta duplicada

Causa comprobada: `host_data_dir` hacía `fs::absolute(ConfigPath / "mod_data" / id)` con
`ConfigPath = sdmc:/switch/dusklight`. En libnx `sdmc:/...` no es absoluta para `std::filesystem` (no empieza por `/`)
y `absolute()` le antepone el directorio actual: `sdmc:/switch/dusklight-h37-c4/sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer`,
que no se puede crear. Pasa con cualquier carpeta del NRO, también con `/switch/dusklight/`. El mismo patrón estaba en
la contención de `http.cpp` (`normalized_absolute`) y, con efecto distinto, en tres `fs::absolute` de `natives.cpp`
(directorio de datos, ruta y directorio de la biblioteca del mod).

- `src/dusk/mount_path.hpp` (sin dependencias del juego): `is_mount_qualified`, `absolute`, `canonical`,
  `normalized_absolute`, `persistent_data_dir` y las reglas de contención. `data::absolute_path` y `data::canonical_path`
  delegan en él: el comportamiento para rutas normales no cambia.
- `host_data_dir` devuelve exactamente `sdmc:/switch/dusklight/mod_data/<id>`, sin depender del cwd ni de la carpeta de
  la candidata.
- HTTP acepta una ruta absoluta con montaje y conserva todas las restricciones: solo dentro del scratch o de los datos
  del mod, sin escapar con `..`, sin otro montaje, y nunca dentro de `<scratch>/downloads` (staging). Antes una ruta
  `sdmc:/...` ni siquiera pasaba `is_absolute()`: un mod en Switch no podía descargar a su carpeta de datos.
- Revisados y sin cambio: `main.cpp` (ruta del ejecutable, host), `bundle_disk.cpp` (usa `fs::relative` sobre rutas del
  paquete) y los usos de `weakly_canonical` de `data.cpp` (ya mount-aware).

`test-mount-paths.sh` (ASan/UBSan): reproduce literalmente `cwd/sdmc:/...`, comprueba la ruta corregida con cwd
`sdmc:/switch/dusklight-h37-c4` y con un cwd de host, `.`/`..`, rutas de host normales, rechazo de otro montaje
(`romfs:`, `save:`), de hermanos con prefijo común, de `..` que escapa, del staging y de rutas no absolutas.

### 18.2 Los dos crashes de Randomizer

Conclusión comprobada: no es el teardown. `GetRandomizerPath()` llamaba a `host->data_dir` (que deja `*out = nullptr` si
falla), después a `host->fail` —que **vuelve**: marca el mod como fallido y el cargador lo desactiva en el siguiente punto
seguro— y seguía con `reinterpret_cast<const char8_t*>(nullptr)` convertido en `std::filesystem::path`: caída en la
instrucción siguiente. Los dos logs terminan justo tras `failed: Failed to get data directory (1)`; ninguno llega a
`deactivating mod` ni a `all mods unloaded`, así que `deactivate_mod`, `unload_native` y los 92 hooks no llegaron a ejecutarse.
El orden de `modules_mod_deactivating`, `mod_shutdown`, `modules_mod_detached` y `unload_native` y el `dlclose` diferido
(`drain_retired_natives`, un tick después) se revisaron y **no se tocaron**: no hay evidencia contra ellos.

- Parche `platforms/switch/patches/randomizer/0005-stop-safely-without-a-data-directory.patch` (el submódulo no cambia:
  mismo flujo que 0001–0004). Las cinco funciones `GetRandomizer*Path` devuelven `std::optional`; `nullopt` si el
  host rechaza, o dice OK con puntero nulo o vacío (una ruta vacía sería el cwd). `fail` se llama una vez; todo llamador
  se detiene: configuración solo en memoria y sin escribir, sin listar semillas ni presets, sin generar, sin diálogos.
  Nada lanza excepciones a través de la frontera C.
- `HostService::fail` queda documentado: devuelve; el código debe dejar de usar lo que falló.
- `test-randomizer-paths.sh`: aplica la serie de parches y ejecuta el `paths.cpp` real con un host de prueba (ASan/UBSan):
  ruta válida (una sola vez, sin `fail`), error, OK con puntero nulo, OK con ruta vacía, host sin función, cinco funciones
  sin ruta, `fail` exactamente una vez y `data_dir` una vez.

No se ha podido compilar Randomizer entero aquí (dependencias de Aurora/SDL): los puntos de uso de rutas de
`ui/config_store.cpp`, `ui/rando_config.cpp` y `randomizer_context.cpp` están revisados a mano y la compilación completa
la hace el build local de `build-h37-test-mods.sh`.

### 18.3 Fallos en ejecución (`fail_mod` tras el arranque)

El guard cerraba la ventana de arranque a los 600 ticks y `pending.json` solo cubre carga, constructores e inicialización:
un `fail_mod` posterior no dejaba rastro y el mismo mod se cargaba otra vez.

- `Category::RuntimeFailure` (`runtime_failure`). Cuando un mod **ya iniciado** (`mod.initialized`) falla por primera vez
  con el arranque terminado, `ModLoader::notify_mod_failure` llama a `BootGuard::record_runtime_failure`: incidente
  (mod, etapa `runtime`, mensaje) y cuarentena, ambos en disco **antes** de la baja. Sin escrituras por frame; sin
  duplicados mientras siga en cuarentena; no es `CrashDuringInit`; un fallo durante `mod_initialize` sigue siendo el
  `InitializeFailure` de `activate_mod` (no se duplica).
- El siguiente arranque no carga el mod (la misma comprobación de cuarentena de `load_native_if_present`). El jugador lo libera
  como antes: apagarlo y volver a encenderlo (`on_enabled_changed`); `Next Start Without Mods` no cambia.
- Hilos: el servicio de hosts, el registro de servicios y la cola del cargador son de un solo hilo (el registro no tiene
  cerrojos), y Randomizer genera semillas en un hilo propio. `HostService::fail` desde otro hilo se encola bajo un cerrojo
  (`ModLoader::report_failure`) y se aplica al principio del siguiente tick; desde el hilo del cargador es `fail_mod`
  directo. Documentado en `host.h`.
- No se afirma atribuir cualquier caída: solo los fallos que el propio mod identificó por `fail_mod`.
- `boot_guard_test` (`test-boot-guard.sh`, ASan/UBSan, procesos hijo que abortan): un fallo en ejecución aparece en incidentes
  y en cuarentena, una caída posterior no lo convierte en `CrashDuringInit` ni en arranque sin mods, el siguiente arranque ve
  la cuarentena, `release` lo permite, no se duplica y `Next Start Without Mods` sigue funcionando.

La prueba de ciclo de vida completa (`fail_mod` → sin más `update` → `Disable` en el siguiente tick → hooks y callbacks
retirados → imagen nativa retirada un tick después) necesita el `ModLoader` entero y no pudo ejecutarse aquí; el código
no se ha modificado, solo se ha leído (§18.2).

### 18.4 Mods del catálogo

Lo que añade H37-c5 es lo mínimo que se demuestra necesario, a partir de los `.dusk` (biblioteca `linux-aarch64`, la que
toma el cargador), no de las listas del resumen:

| mod | imports | de ellos, antes ausentes | hooks por nombre | antes ausentes | ¿carga? |
|---|---|---|---|---|---|
| Twilit Essentials 2.4.2 | 920 | 33 (26 del juego, 4 libc/libm, 3 libstdc++) | 19 (20 registros) | 4 | **no**: `PT_TLS` + 7 `TLSDESC` |
| Dawnlight 3.8.1 | 1049 | 31 (29 del juego, `fmodf`, `sched_yield`) | 27 (36 registros) | 5 | **no**: `PT_TLS` (0xdd0 bytes) + 7 `TLSDESC` |
| ALBW 0.3.0 | 987 | 11 (10 funciones del juego, `g_fpcPf_ProfileList_p`) | 61 (66 registros) | 25 | sin bloqueo estructural conocido; pendiente del ELF final y de hardware |

**Hallazgo nuevo**: la auditoría de H37-c3 miró imports y hooks, no la estructura. Twilit Essentials y Dawnlight tienen
`PT_TLS` y relocaciones `R_AARCH64_TLSDESC`, y el cargador las rechaza **por nombre** (§3, §3.1: el cargador no provee bloques TLS).
Exportar sus símbolos es necesario pero no suficiente: no cargarán hasta que el cargador soporte TLS o los mods se
reconstruyan sin `thread_local`. Soportarlo bien (un resolvedor TLSDESC por hilo, memoria por hilo) es una función del
cargador, no un cambio de exportaciones, y no se ha hecho aquí. ALBW no tiene `PT_TLS`: queda pendiente del ELF final y de hardware, no es una promesa de que cargue.

Imports, en `mod_required_symbols.txt` (generado con `gen_required_symbols.py --merge`, que añade un mod sin los paquetes
de los demás; las líneas de los cinco mods anteriores son idénticas) y exportados por nombre por `ModAbiExports.cmake`:
- todos los símbolos del juego que faltaban están definidos fuera de línea en las fuentes (`dMeterMap_c::ctrlShowMap`,
  `dEvt_control_c::commonCheck/entry/talkXyCheck`, `dEvent_manager_c::order/getEventIdx`, `Z2SeqMgr::startBattleBgm`,
  `daAlink_c::proc*/keepItemData/…`, `fpcSCtRq_Request`, `J3DShapeDraw::draw`, `J2DPrint::printReturn`,
  `j3dTevSwapTableTable`, `g_fpcPf_ProfileList_p`, …): solo les faltaba figurar en la lista;
- `fmod`, `fmodf`, `fprintf`, `isalnum`, `sched_yield`, `std::filesystem::current_path` y
  `directory_iterator::{increment,ctor}` son raíces `--undefined` de la toolchain (si algún nombre no existe en
  devkitA64, el post-link lo dirá);
- `__isoc23_sscanf/strtol/strtoul` ya los define `switch_mod_libc.cpp`; `_Unwind_Resume` va por la tabla de runtime.

Hooks por nombre que desaparecían por inlining (cada uno es estático con un solo llamador, o miembro llamado desde su
propio fichero; GCC lo inlinea y `--gc-sections` descarta la copia; `DUSK_NOINLINE` y nada más, como en H37-c2):

| mod | `DUSK_NOINLINE` |
|---|---|
| Twilit Essentials | `dMenu_Collect2D_c::setHeartPiece`, `canAutoSave` (`src/dusk/autosave.cpp`), `d_a_b_gnd.cpp#damage_check`, `d_a_e_th.cpp#damage_check` |
| Dawnlight | `e_hb_s_damage`, `e_gb_attack_1`, `e_gb_attack_2`, `e_db_s_damage` |
| ALBW | `daE_KK_c/TT_c/GI_c/FZ_c::setActionMode`; `d_a_e_mf.cpp#small_damage`, `e_po_damage`, `e_cr_s_damage`, `d_a_e_s1.cpp#e_s1_damage`, `d_a_e_bs.cpp#e_bs_damage`, `d_a_e_fs.cpp#e_fs_damage`, `d_a_e_sf.cpp#damage_check`, `d_a_e_sf.cpp#e_sf_s_damage`, `d_a_e_kr.cpp#e_kr_damage`, `d_a_e_rdb.cpp#e_rdb_damage`, `e_mm_damage`, `b_bq_damage`, `b_bq_wait`, `e_fm_down/normal/fight_run/damage_run/stop/n_fight/f_fight/fire` |

`dMeter2Draw_c::drawOxygen` (Dawnlight) no se marca: no está inlineada, **nadie la llama** en el build de Dusklight
(`IF_NOT_DUSK` en `dMeter2_c::moveOxygen`; llama a `publishOxygen`). Se conserva como raíz en
`platforms/switch/mod_hook_roots.txt` (leído por `ModAbiExports.cmake` igual que `mod_abi_pinned.txt`): el hook carga y no
se dispara, como en el escritorio. El espacio de funchook de los 34 objetivos solo se puede comprobar con el ELF final.

`test-hook-targets.sh` incorpora las 23 unidades nuevas y audita las 34 (necesita `aarch64-linux-gnu-g++`; no se pudo ejecutar aquí).

### 18.5 Capacidad y combinaciones

Un hook ocupa una página de trampolín (§6.2) y una arena son 64 páginas en un objeto CodeMemory. Cada mod declara
sus objetivos por nombre, por función y por puntero a miembro; el recuento de registros (`package_audit.py`) es una cota
superior de lo que instalará:

| mod | por nombre / función / miembro | páginas (cota) | objetos CodeMemory |
|---|---|---|---|
| Twilit Essentials | 19 / 16 / 199 | 234 | 4 |
| Dawnlight | 27 / 62 / 229 | 318 | 5 |
| ALBW | 61 / 34 / 236 | 331 | 6 |
| Randomizer (ref.) | 17 / – / – | ≈ 92 medidas | 2 |

H37 planea 2 objetos (124 hooks); el pool de CodeMemory del kernel observado es de 10 objetos. Cada uno solo puede probarse
**solo**. Combinaciones no recomendadas: **cualquier pareja de los tres** (9–11 objetos) y cualquiera de ellos con Randomizer
(≥ 6 objetos); Twilit+ALBW comparten 1 objetivo por nombre y ALBW+Dawnlight 2 (el servicio de hooks los compone o los
rechaza como conflicto en tiempo de ejecución). Randomizer, ALBW, Dawnlight, Twilit Essentials, Cosmetics y Roll and Sprint
**no** pueden ejecutarse juntos.

### 18.6 El gate por paquete

`check_h37_candidate.py` audita uno a uno ocho paquetes: obligatorios (loader_lab, Cosmetics, Roll and Sprint,
Randomizer), opcional (luau_runtime) y de catálogo (los tres, por el `id` de su `mod.json`; `--catalog`,
`--require-catalog`; en CMake `ALEKS2_H37_CATALOG_DIR`, `ALEKS2_H37_REQUIRE_CATALOG`). Por paquete informa versión, la
biblioteca que toma el cargador (switch-arm64, o linux-aarch64 en compatibilidad; nunca android/ios/macos/windows), tamaño,
relocaciones, imports encontrados/total y faltantes, símbolos denegados, hooks por nombre
(encontrados/ausentes/ambiguos/no interceptables), servicios obligatorios y opcionales no disponibles, bloqueos
estructurales del cargador (TLS, relocaciones, `DT_NEEDED`), cota de páginas/objetos, y la causa completa de cada rechazo.
Además lista las parejas con exceso de capacidad o objetivos compartidos.

- **READY** = pasó las comprobaciones estáticas contra ese ELF: símbolos, hooks, servicios, estructura. No certifica
  comportamiento en consola. Compatible individualmente no implica compatible junto.
- Sin `--elf/--symdb` (ejecución solo estática) lee los paquetes y juzga lo que puede, y termina
  **FINAL ELF VALIDATION PENDING**: nunca READY.
- Compatible con los scripts existentes: mismas opciones, mismos campos por paquete (el informe pasa a `version: 2` y añade
  `validation`, `tier`, `status`, `capacity_notes`, `combinations`, `not_recommended_together`).

Política por nivel (`package_audit.blocks`, probada en `package_audit_test.py`):

| nivel | ausente | presente y rechazado |
|---|---|---|
| obligatorio | bloquea `ready` | bloquea |
| opcional (luau_runtime) | no bloquea | bloquea |
| catálogo, `--require-catalog` apagado (por defecto) | no bloquea | **no bloquea**: `compatible: false` y todas las causas (TLS, imports, hooks, servicios) quedan en el JSON (`rejections`, `catalog_findings_not_blocking`) y se imprimen como "does NOT block ready" |
| catálogo, `--require-catalog` | bloquea | bloquea |

`--require-catalog` exige que los tres paquetes estén presentes **y** sean compatibles. La ejecución solo estática
(sin ELF) es siempre NOT READY por la validación final pendiente, sea cual sea el nivel.

### 18.7 Pruebas

Ejecutadas aquí (host, sin NRO): `test-mount-paths.sh`, `test-randomizer-paths.sh`, `test-boot-guard.sh`,
`test-indirect-references.sh`, `test-package-audit.sh` (con los tres paquetes reales), `test-host-services.sh`
(21 pruebas, 2 omitidas), `test-mod-services.sh`, `test-link-response-file.sh`, `test-switch-patches.sh`,
`test-dusk-reproducible.sh`, `test-code-arena.sh` (parte A), y el gate de punta a punta contra un ELF AArch64 de prueba
(solo comprueba que el camino de código se ejecuta). `git diff --check` limpio.

Omitidas por falta de herramienta (no cuentan como aprobadas): todo lo que pide `aarch64-linux-gnu-g++`
(`test-symbol-manifest.sh`, `test-hook-targets.sh`, `test-elf-loader.sh`, `test-h37-candidate.sh`,
`test-h37-mods-reproducible.sh`, parte B de `test-code-arena.sh`); `test-hook-conflicts.sh` y `test-switch-io.sh` (cabeceras de
Aurora y SDL3, fuera del alcance de esta sesión); `test-static-mods.sh` (fmt); la prueba de ciclo de vida del `ModLoader`.

Pendientes del ELF final: imports 881/881 de Randomizer y de los tres del catálogo contra el `.dynsym` nuevo; que los 34
hooks existan, sean interceptables y no estén ambiguos tras los `DUSK_NOINLINE`; que `fmod`, `fmodf`, `fprintf`, `isalnum`,
`sched_yield` y los `std::filesystem` se enlacen; que los 17 hooks de Randomizer sigan cumpliéndose.

### 18.8 Protocolo corto de hardware

1. Arrancar con Randomizer apagado: arranca, `loader_lab`, Roll and Sprint y Cosmetics cargan (4 mods, 1 objeto).
2. Activar Randomizer desde Mods: log sin `Failed to get data directory`; existe `sdmc:/switch/dusklight/mod_data/dev.twilitrealm.randomizer/`.
3. Randomizer: generar una semilla, guardar partida, salir, recargar y comprobar la semilla.
4. Forzar un fallo (renombrar la carpeta `mod_data` por un archivo): el mod falla **sin** caer el juego; el siguiente
   arranque lo muestra en cuarentena en Ajustes; apagar y encender lo libera.
5. ALBW, solo (hasta 331 páginas = 6 objetos CodeMemory estimados), con Randomizer apagado: ¿carga? Pendiente de hardware; abrir una partida y comprobar un enemigo con daño propio (p. ej. Bokoblin).
6. Twilit Essentials y Dawnlight: se espera `thread-local storage` en el Dry Run (§18.4); confirmar que lo dicen y no caen.
7. Guardar `.guard/report.json` y el log.

### 18.9 Build local de H37-c5

```
git fetch origin claude/h37-dynamic-mod-loader
git merge --ff-only origin/claude/h37-dynamic-mod-loader     # hash final del informe
git submodule sync
git submodule update --init extern/aurora extern/borealis mods/cosmetics mods/randomizer   # los gitlinks no cambian
# (desde el worktree aislado, como en §15.7)
cmake --preset switch-devkitA64-release-h37 -DALEKS2_BUILD_TAG=H37-c5 -DALEKS2_H37_PACKAGES_DIR=<paquetes oficiales> \
      -DALEKS2_H37_CATALOG_DIR=<carpeta con los tres .dusk del catálogo>
platforms/switch/tools/build-h37-test-mods.sh <paquetes oficiales>   # aplica 0001-0005 a Randomizer
cmake --build --preset switch-devkitA64-release-h37
cmake --build --preset switch-devkitA64-release-h37 --target dusklight_h37_candidate
```

El informe `h37-candidate.json` debe traer, para los cuatro obligatorios, `compatible: true`. Con
`ALEKS2_H37_REQUIRE_CATALOG=OFF` (la configuración prevista) el gate puede quedar READY: Twilit Essentials y Dawnlight
aparecen como catálogo `compatible: false` por TLS sin bloquear, y ALBW depende del ELF final. Con `ON` el gate
fallará mientras haya TLS: es lo correcto, no un error del build.

## 19. H37-c6: Randomizer como módulo switch-arm64 (libstdc++ del juego)

### 19.1 Qué falló en consola (H37-c5)

Con la ruta de datos ya corregida, Randomizer se activaba y el juego caía al abrir *Seed Management*: Data Abort en
`std::locale::operator=` ← `ios_base::_M_init` ← `basic_ios::init` ← `basic_ofstream::basic_ofstream` (módulo, `fstream:799`) ←
`Config::LoadFromFile`. El constructor de `std::ofstream` se compila **en línea dentro del módulo** con las cabeceras de
libstdc++ 13 de Ubuntu; las funciones que llama (`ios_base::_M_init`, `basic_ios::init`) son las del juego (devkitA64,
libstdc++ 16). Las dos versiones no colocan igual los miembros de `ios_base`/`basic_ios`: la `_M_init` del juego lee un
`locale` nulo. No es un locale global sin inicializar y ningún shim lo arregla; todo `ofstream/ifstream/stringstream` del
módulo está igual de roto. La premisa de §2 («mismo ABI de C++») valía para los nombres de los símbolos, no para el layout.

### 19.2 Arreglo

1. **Módulo compilado con el compilador del juego.** `platforms/switch/cmake/SwitchArm64Mod.toolchain.cmake`:
   `aarch64-none-elf-g++` de devkitA64 (de `PATH` o de `$DEVKITPRO/devkitA64/bin`), `-march=armv8-a+crc+crypto
   -mtune=cortex-a57 -fPIC -mno-outline-atomics`, y enlace `-nostartfiles -nodefaultlibs`: **nada** de libstdc++, libgcc,
   libc ni libnx entra en el módulo, y todo símbolo del juego/newlib/libstdc++/libgcc queda sin resolver para el cargador
   (sin `DT_NEEDED`). Se enlazan `crtbegin`/`crtend` del compilador (`__dso_handle`, el `frame_dummy` de `.init_array` y el cero
   que cierra `.eh_frame`, que el `__register_frame` del cargador necesita) y no `crt0/crti/crtn`. `-mno-outline-atomics`: GCC ≥10
   llama a los `__aarch64_*` de libgcc, que el NRO no exporta. No es el toolchain del NRO; no usa `MODULE` del NRO.
2. **Plataforma explícita.** `cmake/ModSDK.cmake`: `DUSK_MOD_LIB_PLATFORM=switch-arm64` (lo fija el toolchain) guarda
   `lib/switch-arm64/mod.so`. Solo se acepta con ese toolchain y un destino AArch64; un nombre de sistema `NintendoSwitch`
   ya no produce una plataforma inventada y otro valor se rechaza.
3. **`build-h37-test-mods.sh`** construye `randomizer.dusk` así si hay devkitA64: **solo** `lib/switch-arm64/mod.so`,
   comprobado por `check_switch_module.py` (paquete con exactamente esa biblioteca, `ET_DYN`, AArch64, sin `DT_NEEDED`,
   sin TLS, sin relocaciones de texto, con imports sin resolver, sin símbolos propios de libstdc++/libgcc, exports de mod). Con
   `SYMGEN` escribe además en `OUT_DIR` la `mod_required_symbols.txt` regenerada con las líneas de ese Randomizer y avisa si
   difiere de la versionada: **los nombres que importa contra libstdc++ 16 pueden diferir de los de Ubuntu**, y el NRO solo
   exporta lo que esa lista nombra. Sin devkitA64 **no** construye Randomizer (§19.5); el linux-aarch64 solo existe para pruebas de host, con una opción explícita y otro nombre.
4. **El cargador y la auditoría eligen la misma biblioteca** (`switch-arm64` antes que `linux-aarch64`; nunca otra): el gate
   exige `switch-arm64` para el Randomizer oficial.

### 19.3 Protección para otros mods

Un módulo `linux-aarch64` que importe `std::ios_base`, `basic_ios`, `basic_ifstream/ofstream/fstream`,
`basic_stringstream/ostringstream/istringstream` (cualquier símbolo cuyo nombre mangled los contenga:
constructores, miembros, vtables, VTT) se rechaza **antes de ejecutar código**, con `denied_symbol` y el motivo:
*libstdc++ ABI not safe … a switch-arm64 library is required*, en `elf_runtime_policy.cpp` (Dry Run, informe JSON) y en
`elf_audit.py` (`loader_blockers` del gate). `ios_base::Init` (el inicializador estático de `<iostream>`) y el resto de
llamadas a la biblioteca no se tocan, y **`std::locale` aislado tampoco**: Cosmetics importa seis símbolos de `locale` (`locale()`, `locale(const&)`, `~locale()`, `locale::id::_M_id`, `locale::facet::~facet`, el typeinfo de `facet`), cargó y se reactivó en consola, y la evidencia del crash apunta a `ios_base`/`basic_ios`; la primera versión de la política (`39b17a0`) lo rechazaba por error y habría impedido que H37-c6 llegara a READY. Sin excepciones por id de mod: runtime y auditor comparten la tabla `abi_policy_names.txt`. Con esta política `luau_runtime` (stringstream/ios_base) seguiría rechazado como linux-aarch64; un módulo `switch-arm64` con esos mismos imports no se rechaza por esta política.
Un `linux-aarch64` sin esas familias (Cosmetics, Roll and Sprint, loader_lab) se comporta como antes. Un módulo
switch-arm64 que usa iostreams aparece en el gate como `UNVALIDATED: uses iostreams … not validated by execution`: nadie lo ha
ejecutado en consola todavía.

### 19.4 Pruebas

Ejecutadas (host): `test-runtime-policy.sh` (política C++ por ABI, ASan/UBSan), `test-abi-policy.sh` (selección de
biblioteca; veredictos y `check_switch_module.py` sobre objetos compartidos AArch64 reales hechos con clang+lld: switch
gana a linux, iostream/locale se rechaza en linux y no en switch, sin la familia no cambia, `ios_base::Init` permitido,
paquete con dos bibliotecas / solo linux / `DT_NEEDED` / TLS / runtime privado / sin imports → rechazados),
`test-mod-sdk-platform.sh` (plataforma explícita y el toolchain contra herramientas de relleno), `test-package-audit.sh` (el gate no
da por compatible un Randomizer linux-aarch64; el oficial se audita por switch-arm64).

**No verificado: necesita devkitA64.** Que `aarch64-none-elf-g++ -shared` enlace un módulo así con estos flags, que
`crtbegin.o`/`crtend.o` existan en esa distribución (el toolchain falla con un mensaje si no), que Randomizer compile contra las
cabeceras de newlib (el enlace ya está probado con herramientas de relleno: una sola orden, §19.6), el contenido real de `mod_required_symbols` del nuevo Randomizer, `test-elf-loader.sh`/`test-symbol-manifest.sh`
/`test-h37-candidate.sh` (piden `aarch64-linux-gnu-g++`) y, sobre todo, que un `std::ofstream` del módulo funcione en consola.
Un caso mínimo con iostreams en `loader_lab` queda por hacer.

### 19.5 Build local de H37-c6 (dos entornos, una carpeta de salida)

`build-h37-test-mods.sh` ya no exige `aarch64-linux-gnu-g++` si no se pide ningún paquete linux-aarch64: se ejecuta dos veces
sobre la **misma** `OUT_DIR`, y la segunda no borra lo de la primera (`SHA256SUMS` lista todo lo que haya al final; el oráculo de
relocaciones se hace solo para lo construido en esa ejecución, con el `readelf` de su toolchain: `aarch64-none-elf-readelf` para
switch-arm64). Randomizer **sin** devkitA64 falla antes de construir nada; solo
`H37_ALLOW_UNSAFE_LINUX_RANDOMIZER_FOR_HOST_TESTS=1` genera el linux-aarch64, con el nombre `randomizer-linux-host-test.dusk` (nunca
`randomizer.dusk`, que es lo que mira el gate) y solo para pruebas de host. Sin `H37_TEST_MODS` se piden todos los paquetes: hace falta
el toolchain Linux **y** devkitA64.

Ruta con espacios (`D:\path\to\My Projects\…`): el flujo switch-arm64 la rechaza **antes de configurar**, con estos
comandos. En la ventana de Windows (cmd), una vez por sesión, con las rutas reales:

```
subst X: "D:\path\to\My Projects\<carpeta del repositorio>"
subst Y: "D:\path\to\My Projects\<carpeta de salida de los paquetes>"
```

El paso de Docker usa la misma carpeta de salida montada como volumen (sin espacios en la ruta del contenedor). Orden:

```
# 1) Docker/Linux (dentro del contenedor; /work = el repositorio, /out = la carpeta de salida):
H37_TEST_MODS="loader_lab template_mod rollandsprint cosmetics" FMT_SRC=<fmt 12.1.0> COSMETICS_SRC=<cosmetics> \
  /work/platforms/switch/tools/build-h37-test-mods.sh /out

# 2) Windows, Git Bash (cmake y ninja en PATH; devkitA64 en PATH o $DEVKITPRO):
export DEVKITPRO=/c/devkitPro            # o la ruta de MSYS que corresponda
cd /x
H37_TEST_MODS=randomizer SYMGEN=<symgen.exe> FMT_SRC=<fmt, sin espacios> \
  platforms/switch/tools/build-h37-test-mods.sh /y
#   "built randomizer (switch-arm64 …)" y "ok /y/randomizer.dusk". Si avisa "needs different exports":
#   copiar /y/mod_required_symbols.txt sobre platforms/switch/mod_required_symbols.txt y commitear ANTES del NRO.

# 3) NRO, una vez (los paquetes están en Y:\):
cmake --preset switch-devkitA64-release-h37 -DALEKS2_BUILD_TAG=H37-c6 -DALEKS2_H37_PACKAGES_DIR=Y:/ \
      -DALEKS2_H37_CATALOG_DIR=<3 .dusk> -DALEKS2_H37_REQUIRE_CATALOG=OFF
cmake --build --preset switch-devkitA64-release-h37
cmake --build --preset switch-devkitA64-release-h37 --target dusklight_h37_candidate
# al terminar: subst X: /d  y  subst Y: /d
```

Los bytes de los paquetes no dependen de la letra de unidad: las rutas se mapean con `-ffile-prefix-map` (`/dusklight`, `/h37-mods`).
`test-build-mods-environments.sh` cubre esta separación con herramientas de relleno. En consola: abrir *Seed Management*,
generar, guardar y recargar una semilla; `h37-candidate.json` debe dar a Randomizer `platform: switch-arm64`, `compatible: true`
y la nota `UNVALIDATED`.

### 19.6 Correcciones tras el primer build real (Windows, devkitA64)

- **Regla de enlace partida.** `SwitchArm64Mod.toolchain.cmake` construía `CMAKE_<LANG>_CREATE_SHARED_LIBRARY` con `set()` y varias
  cadenas: CMake las unió con `;` y leyó cada `;` como fin de comando, así que la regla eran tres órdenes (`g++ <flags>`,
  `-o x`, `crtbegin <objetos> crtend`) y `ld` decía «no input files». Ahora es **una** cadena (`string(CONCAT)`), también para
  `CREATE_SHARED_MODULE`:
  `g++ -fPIC $LANGUAGE_COMPILE_FLAGS $ARCH_FLAGS $LINK_FLAGS -shared $SONAME_FLAG$SONAME -o $TARGET_FILE "crtbeginS.o" $in $LINK_PATH $LINK_LIBRARIES "crtendS.o"`.
  `test-toolchain-link-rule.sh` configura un MODULE mínimo y lee `rules.ninja`; contra `9e32a16` falla con las tres órdenes.
- **fmt del host.** `_mod_add_fmt()` ejecutaba `find_package(fmt)` aunque se diera `-DFETCHCONTENT_SOURCE_DIR_FMT`, y encontraba el fmt
  de MinGW por el prefijo del compilador. Con esa variable ya no se busca en el host: se usa la fuente explícita (fmt 12.1.0). Sin
  ella, igual que antes. `test-mod-sdk-fmt.sh`: con un fmt de host «equivocado» y una fuente explícita, los includes del módulo son solo los de la
  fuente; contra el `ModSDK.cmake` anterior falla.

## 20. H37-c8: excepciones del módulo y generación de hints

### 20.1 Evidencia de hardware de c6 y c7

Las cuatro caídas durante *Generating Hints* tienen la misma pila del hilo generador. En c7 el hilo sí tiene la pila
de 8 MiB de `0006`, por lo que el tamaño de pila queda descartado como causa. La pila resuelta contra el ELF y el
`randomizer.so` de c7 es:

```
Text::Capitalize (text.cpp:86)
GenerateAllHints (hints.cpp:1246)
Randomizer::GenerateWorlds (randomizer.cpp:187)
std::locale::locale("en_US.iso88591")
std::__throw_runtime_error
__cxa_throw -> std::terminate -> abort -> __libnx_exit
```

Switch no instala esa locale. `Text::Capitalize` tenía un `catch (runtime_error)` y una ruta de respaldo, pero el
paquete real de c7 contiene `.eh_frame` y `.gcc_except_table` **sin `PT_GNU_EH_FRAME`**. El cargador localiza
`.eh_frame` mediante ese program header; al faltar, no llama `__register_frame`. El unwinder no encuentra el handler
del módulo y termina el proceso. La pérdida del dispositivo Vulkan y la caída posterior en `_write_r` ocurren durante
la salida de libnx y son consecuencias.

### 20.2 Corrección y gates

- El toolchain switch-arm64 enlaza con `-Wl,--eh-frame-hdr`. El paquete debe contener `PT_GNU_EH_FRAME`; entonces el
  cargador registra sus FDE antes de constructores y funciones del mod.
- `elf_audit.py`, `package_audit.py` y `check_switch_module.py` rechazan cualquier módulo con `.eh_frame` sin ese
  program header. Así el paquete de c7 deja de pasar el gate.
- `0007-locale-free-hint-capitalization.patch` elimina la dependencia de locales instaladas. Convierte ASCII y los
  rangos minúsculos de Latin-1 de forma determinista, sin construir `std::locale` y sin lanzar una excepción durante
  una semilla válida.
- `test-toolchain-link-rule.sh` exige `--eh-frame-hdr`; `test-randomizer-locale.sh` aplica la pila real de parches,
  comprueba que `Text::Capitalize` no usa locale/facets y prueba las conversiones ASCII/Latin-1.

Antes de otro NRO, el `randomizer.dusk` nuevo debe pasar `check_switch_module.py`, y `readelf -lW mod.so` debe mostrar
`GNU_EH_FRAME`. La prueba de consola es generar una semilla completa; la línea `seed generation started` debe ir
seguida del resultado de generación sin salida de libnx ni crash report nuevo.
