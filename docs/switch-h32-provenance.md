# H32: cierre de procedencia (souldbminer)

Hito dedicado solo a esto: ninguna optimización, ni DRS, shaders, navegador, mods o funciones
nuevas. Partió de H31-c1 (validado en hardware) con Mesa 26.2.3 reparado de H30 y el memo de
pipelines de 1024 entradas, y los conserva.

Criterio de cierre: no basta con que el detector marque cero. Debe quedar cero código atribuible a
souldbminer sin permiso, y cada coincidencia que quede debe estar demostrada como **upstream**,
**trivial**, **API obligatoria** o **implementación independiente**.

## Cómo se mide

Dos instrumentos, porque cada uno se equivoca en una dirección distinta
(`platforms/switch/tools/souldb-residue.py`, con su libro de justificaciones
`souldb-ledger.json`):

1. **Contenido** (el de siempre): líneas que `v2.0.2 → nx/main` añade y que siguen literales en el
   árbol. Techo con falsos positivos; solo ve `src/`, `platforms/` e `include/`.
2. **Historial** (`--blame`, nuevo): líneas cuyo último escritor es un commit suyo
   (`git blame -w -M -C`), en el repositorio y en `extern/aurora`. Lee el árbol de trabajo, así que
   comprueba una reescritura antes de commitearla. Descubrió que `borealis_io_switch.cpp` marcaba 188
   por contenido pero 264 por historial: se había «reescrito» editando su archivo y la estructura
   seguía siendo la suya.

Ninguno de los dos mide autoría por sí solo. El libro de justificaciones sí: para cada archivo que
conserva líneas suyas dice **por qué** pueden quedarse, con `max_lines` para que no crezca sin
avisar, y `refs` (la interfaz o la referencia pública que el archivo sigue) para que se pueda leer
la prueba en vez de fiarse (`--explain ARCHIVO` imprime lo que las referencias no contienen).
La herramienta sale con error mientras algo esté pendiente.

## Antes y después

| | H31-c1 | H32 |
|---|---:|---:|
| Contenido (`--min 10`) | 3 164 de 6 264 líneas (51 %) | **473 de 6 172 (8 %)** |
| Historial, repositorio (líneas / sustantivas) | 4 770 / 3 412 en 68 archivos | 1 128 / 589 en 77 archivos (con Aurora) |
| Historial, `extern/aurora` | 458 / 298 en 36 archivos | incluido arriba |
| Pendientes sin justificar | todo | **0** |

«Sustantivas» = sin llaves, `#endif`, líneas de ≤3 caracteres ni líneas que ya están en upstream.
Las cifras de historial siguen siendo un techo: la detección de copias atribuye a su commit toda
línea idéntica, aunque la haya escrito ahora, de la única forma posible.

Por clase (archivos / líneas / sustantivas): **api** 22 / 579 / 306, **trivial** 41 / 328 / 190,
**independiente** 14 / 221 / 93, **upstream** 0.

## Qué se hizo, con su origen comprobado

### Se borró (no lo usaba el build)

- `patches/dawn-switch.patch` (974 líneas): camino muerto. `deps/dawn` es `encounter/dawn@1155e0ed`,
  que ya trae la plataforma Switch; el parche completo solo se aplicaba a un clon de Google sin tocar.
- `patches/aurora-switch.patch` y `cmake/SwitchAuroraPatch.cmake`: no hacían nada
  (`BackendBinding.cpp` ya trae el NWindow; la normalización de `ConvertibleStatus` tampoco hacía
  falta con el Dawn que Aurora fija: `lib/aurora.cpp` vuelve a ser el de upstream).
- Helpers de CMake duplicados (`ApplyDawnDepsPatch`, `ApplyTracyPatch`, `ApplyDawnSwitchPatch`,
  `ApplySDLSwitchPatch`, `SwitchNRO`), `switch_egl_stub.cpp`, `switch_input_hid.cpp`,
  `switch_perf.cpp`, `gfx_backend.hpp`, `input.hpp`, `perf.hpp`, `icon-midna.jpg` (su icono
  original, sin referencias): ninguna función de ellos tenía llamadores.

### Parches de terceros

| Pieza | Antes | Ahora |
|---|---|---|
| **abseil** (dentro de Dawn) | `dawn-deps-abseil.patch`, suyo | **No se lleva parche.** `encounter/dawn` incluye `patches/abseil-cpp.patch` y lo aplica su propio CMake cuando `DAWN_PLATFORM_SWITCH` está activo. Es el mismo texto (se diferencian en una palabra del contexto, por la versión de abseil); upstream público |
| **Dawn** (capas de instancia) | delta de 3 hunks extraído de su parche | `dawn-optional-layers.patch`, propio: `vkEnumerateInstanceLayerProperties` puede ser nulo (Mesa lo deja al loader) y su recopilación se omite. El hunk de `BundleLibraries` (`-ldl`) desaparece porque CMake sí deja `CMAKE_DL_LIBS` vacío (`cmake/SwitchOptions.cmake`) |
| **Tracy** | `tracy-switch.patch`, suyo | `tracy-horizon.patch`, propio: dos ramas de plataforma en `TracySystem.cpp`. (Comprobado: el cliente de Tracy se compila aunque el perfilado esté apagado) |
| **SDL3** | `sdl3-switch.patch` (1 385 líneas) sobre SDL 3.4.10 | **`devkitPro/SDL`, rama `switch-sdl-3.4`**, commit `c329016c` (SDL 3.4.0 + soporte Switch público, zlib), con su hash fijado, más una superposición propia (abajo) |

#### SDL3

La rama pública de devkitPro ya trae audio (audren), reloj, temporizador, energía, sistema de
archivos, hilos y las definiciones de plataforma. Su código era el mismo linaje que el de su
backend de audio (la misma `arConfig` y las mismas funciones; solape medido de lo suyo con lo
público: audio 53 %, joystick 32 %, vídeo 23 %). Dos partes no sirven tal cual y se sustituyen
(`cmake/SwitchSdlOverlay.cmake`, `platforms/switch/sdl`):

- **Vídeo**: el de devkitPro dibuja con EGL/GLES (Dawn no lo usa) y además mete táctil, ratón y
  teclado en la cola de eventos, lo que duplicaría el puente táctil de `src/dusk`. El nuestro es una
  ventana sin GL: una pantalla, una ventana solo a pantalla completa, el cambio dock/portátil cada
  500 ms y la petición de salida del applet. Se escribió desde la interfaz de SDL y de libnx.
- **Joystick**: el de devkitPro expone ocho mandos a la vez; el juego lee el jugador uno, que es el
  mando «por defecto» de libnx (modo portátil, Pro Controller o Joy-Con fusionados). Mismo
  comportamiento que antes, escrito de nuevo; el emparejado de botones (L/R como gatillos, ZL/ZR
  como hombros, MISC1-4 repetidos) se conserva porque los controles validados dependen de él.

Tres cambios a archivos públicos de devkitPro, hechos por el script con comprobación de que el texto
es el esperado: canales como máximo dos (el juego puede pedir 6 u 8 y el mezclador solo sabe uno o
dos), `WaitDevice` que espera de verdad a un búfer libre (el de devkitPro devuelve de inmediato y
`PlayDevice` tira el bloque si ambos siguen ocupados: un chasquido), y prioridades de hilo ignoradas
(devkitPro baja todo lo que no es «alta» a 0x3B, por debajo del 0x2C de libnx, incluido el hilo de
audio). Dos cosas del parche anterior se dejan de llevar: la cota de muestras F32→S16 en NEON
(redundante: la instrucción satura) y las guardas de `SDL_systhread`/`SDL_systime` (están en la
rama de devkitPro).

### Hallazgos de la auditoría que cambian cosas

1. **El ayudante de parches decía «ya aplicado» sin estarlo.** Preguntaba a GNU patch si podía
   revertirlo con `-R --dry-run`; en modo batch, si no está aplicado, patch descarta `-R` y responde
   éxito. El primer configure de esta rama dejó Dawn *sin* el parche de capas, lo que en consola habría
   sido «Couldn't get proc». Ahora se comprueba con una prueba en seco hacia delante, y cada parche
   puede nombrar un texto que el archivo debe contener después (`MARKER_FILE`/`MARKER_TEXT`).
   `platforms/switch/tools/test-switch-patches.sh` lo cubre.
2. El mismo ayudante hace que FetchContent vuelva a ejecutar el parche cuando cambia el parche o el
   script.
3. `CMAKE_DL_LIBS` valía `dl` pese al valor vacío de la caché (el módulo de plataforma de CMake lo
   fija como variable normal); eso es lo que obligaba a un hunk de Dawn y a un bucle que quitaba `dl`
   de siete targets. Solo `funchook` lo nombra en literal, y es lo único que se limpia.

### Plataforma (`platforms/switch`)

- `CMakeLists.txt`, `cmake/DevkitA64Libnx.cmake`, `SwitchOptions.cmake`, `SwitchDeps.cmake`,
  `CMakePresets.json`: reescritos sobre los macros de devkitPro (`nx_generate_nacp`, `nx_create_nro`).
  **El autor del NACP ya no es «Twilit Realm and Souldbminer»**: es `Twilit Realm, Aleks` (cambio de
  una línea en `platforms/switch/CMakeLists.txt` si se prefiere otro).
- `borealis_{io,data,net,ws,file_select}_switch.cpp`: reescritos desde las interfaces. `io` usa
  `SDL_IOStream` real (el `handle()` del encabezado ya es válido) y resuelve `pread` con `lseek+read`
  bajo un cerrojo, y el `rename` que no reemplaza con quitar-y-renombrar.
- Arranque de libnx (`switch_startup.cpp`), colocación de un mando en el puerto 1
  (`switch_pad_port.cpp`), carpetas de la tarjeta (`switch_fs.cpp`): reescritos; salen de
  `m_Do_main.cpp`.
- `switch_posix_shim.cpp`: sin cambios de código. Cada función es una firma de POSIX/newlib con el
  valor de retorno que da la función real cuando no puede hacer su trabajo (clase `api`).

### Juego (`src/`)

- `profiler.hpp` reimplementado (misma API para el benchmark, el overlay y los marcadores);
  `ImGuiMenuTools.cpp` (tabla del profiler) también.
- `iso_validate.cpp` (cabecera del disco), `ui/prelaunch.cpp` (búsqueda de la imagen), `logging.cpp`
  (nivel de registro), `data.cpp` (se retira su ruta de caché fija: la da el gestor de datos),
  `config.cpp` (vuelve a upstream salvo que el reemplazo del archivo pasa por `borealis::io`),
  `livesplit.*` y `mods/svc/gfx.cpp` (vuelven a ser los de upstream).
- Resolución: `resolution_ladder.hpp` (alturas exactas) y `switch_display.*` (qué significa «auto»
  y su reajuste al acoplar o desacoplar).
- Enumeración `InterpQuality` y fila de ajustes: ahora es un `config_enum_select` genérico en vez de
  un bloque copiado.

### Aurora (`extern/aurora`)

`PassBindFilter`, `UniformRepeat`, `DrawEvent`, `sqlite::open_cache`, `LargeStackThread`,
colocación de hilos por tabla, y `push_gx_draw` dividido en funciones; se retira la caché de
`ShaderInfo` por hash (la sustituye el memo de H31, que es nuestro) y `submit_raw_draw`, que no tenía
llamadores. **Consecuencia documentada:** con el memo apagado el camino ya no es el de H30 (ahora
construye `ShaderInfo` en cada cambio de configuración); con el memo encendido, que es el
predeterminado, no cambia nada.

### Metadatos

`.gitmodules` apunta `extern/aurora` a `encounter/aurora` (antes a su repositorio),
el README raíz y `docs/building-switch.md` (contaban que no había SDL, que usaba NXVK, deko3D y
SaltyNX) están reescritos, `.gitignore` pierde dos líneas de su espacio de trabajo.

## Coincidencias conservadas y por qué

Todas en `platforms/switch/tools/souldb-ledger.json` con su razón. En resumen:

- **api (22 archivos):** configuración de CMake (comandos y opciones que no son nuestros);
  las cinco interfaces de borealis (`io`: 65 de sus 80 líneas sustantivas están literalmente en el
  encabezado o en la implementación pública de borealis, y las otras 15 son `#include`, `if (fd < 0)`
  y `errno = ...`); `switch_posix_shim.cpp` (firmas de POSIX); las funciones del SDK de GX que Aurora
  no tenía (`GXSetScissorBoxOffset`, `GXSetCopyClamp`); la superficie NWindow de Dawn.
- **trivial (41):** guardas `#ifdef __SWITCH__`, marcadores `DUSK_PROFSCOPE` de una línea,
  condiciones `!fancy_recording() ||`, la constante `kDrawBatchSize`, los `lock_guard` de la caché de
  texturas, la solución al macro `_C` de newlib.
- **independiente (14):** lo reescrito arriba; lo que coincide son los nombres de la API que otros
  archivos usan y los tipos del propio archivo.

Lo que **no** se puede demostrar ni se pretende: que la *idea* de varias optimizaciones (filtrar
binds repetidos, repetir uniforms idénticos, no interpolar partículas en modo rápido) no se le
ocurrió a él. Lo que se retiró o reescribió es su expresión.

## Verificación hecha

- Compilación completa del NRO desde un configure limpio de dependencias (Dawn, SDL, Tracy, ...
  descargados y parcheados por CMake).
- Banderas de compilación idénticas a las de H31 en tres objetos de muestra (`m_Do_main`,
  `command_processor`, `JKRHeap`) y línea de enlace con las mismas banderas y los mismos archivos `.a`; diferencia de símbolos con el ELF de H31 revisada: desaparecen los renderizadores de
  SDL (GLES2, software), `switch_egl_stub` y lo retirado; aparecen lo reescrito.
- Pruebas host: `test-pipeline-memo.sh`, `test-benchmark-report.sh`, `test-switch-patches.sh`,
  `test-profiler-ladder.sh`. `tools/check-mod-abi.py v2.0.3` pasa.

## Qué mirar en consola (no verificable sin hardware)

SDL es lo único que toca el hardware y cambió casi entero.

1. **Audio**: sin chasquidos al pasar de menús al juego ni en escenas con mucho sonido; con el ajuste
   de sonido envolvente (6/8 canales) sigue sonando en estéreo.
2. **Mandos**: portátil, Pro Controller, dos Joy-Con y un Joy-Con de lado (Flip Grip); conectar un
   segundo mando con el primero emparejado; desconectar y volver a conectar el del puerto 1; los
   botones L/R/ZL/ZR en la pantalla de configuración de mandos.
3. **Ventana**: pasar de portátil a base y de vuelta (la resolución «auto» debe seguir 720 ↔ 1080, y
   540 con Flip Grip) y salir por el menú HOME.
4. **Táctil** del compañero (no debe duplicarse), vibración, giroscopio.
5. **Guardado** y ajustes (se escriben ahora a través de `borealis::io`), lista de disco al arrancar.
6. **Rendimiento**: benchmark F_SP122 contra H31 (29,27 FPS, Execute 32,3 ms, DrawRec 9,5 ms): ruido
   razonable; el JSON del benchmark y el overlay del profiler siguen igual.

## Para decidir (no se tocó sin pregunta)

- **Autor del NACP** (arriba).
- **Historial de git.** El historial de `aleks2` y el de `extern/aurora` contienen sus commits, es
  decir, su código antiguo, aunque el árbol ya no lo tenga. Para publicar sin eso hace falta un
  historial nuevo (un commit inicial del árbol actual sobre upstream). No se hizo ni se hará sin
  autorización.
- **Fork de Aurora.** El submódulo apunta a un commit local de la rama `aleks2` que no existe en
  `encounter/aurora`; para una publicación hace falta publicar ese fork y poner su URL en
  `.gitmodules`.
- **Validación de discos comprimidos (corregido antes de la prueba en hardware).** El validador de
  Switch miraba los primeros bytes del archivo con stdio y rechazaba RVZ/WIA/GCZ («convierte en
  Dolphin») aunque `prelaunch.cpp` los ofrece y `disc_container.cpp` los lee. Venía del commit
  `cc9f0e3674` (27/09), que lo separó del lector cuando éste iba a reemplazarse por uno sin
  compresión; `ed0ea227b4` (28/09) devolvió el lector comprimido y no se revisó el validador. Ahora
  `platforms/switch/source/disc_header.cpp` abre la imagen con `disc_container` y lee los primeros 0x20
  bytes **lógicos** del disco; ISO, RVZ, WIA y GCZ pasan por la misma comprobación de id, consola,
  disco y revisión, y CISO/WBFS/NFS/TGC se rechazan con su explicación. No hay decodificación nueva ni
  hash completo. Prueba: `platforms/switch/tools/test-disc-header.sh`.
- Todo lo ajeno de antes (`docs/cross-save-1.0.md`, `docs/switch-v1-roadmap.md`, tus cambios en
  `docs/switch-h28-diagnostics.md` y `platforms/switch/README.md`) sigue sin commitear y sin tocar,
  salvo las líneas de `platforms/switch/README.md` descritas arriba.
- El Mesa 26.2.3 reparado con `ranlib` sigue pendiente de sustituir antes de una V1 pública.

## Resultado

Candidato único **H32-c1**, **validado en hardware el 1 de octubre de 2026** (ver «Validación en hardware»). Sustituye al primer H32-c1 (`2dbca6f7cc`), que
no llegó a probarse: tenía el validador de discos desconectado del lector (ver arriba).

| | |
|---|---|
| Commit | `b84a849829` (Aurora `d3f21d9`) |
| Build ID | `H32-c1-b84a849829` (dirty=0) |
| `SWITCH-TEST-H32/dusklight-h32-c1.nro` | 59 705 123 B, SHA-256 `0dce5ee805558823e3ffae3fbdaac93734cec8246b9d1849393a0a095d785210` |
| `dusklight.elf` | 911 704 776 B, SHA-256 `f44b0b5b58ea88fedfc1d870536ea8d040cbe7cb041d8c797fa864f2fa1eecdc` |
| `verify-nro.py` | «NRO built from this ELF»; icono, NACP y romfs presentes; título «Dusklight» |

Pruebas en PC: `pipeline_memo`, `benchmark_report`, `switch_patches`, `profiler_ladder` y
`disc_header` pasan; `testkit/run-disc-test.sh` (lector de contenedores, modo rápido: ISO, RVZ zstd/
lzma/bzip2/none, GCZ y WIA lzma) pasa; `check-mod-abi.py v2.0.3` correcto. `disc_header` comprueba
que RVZ (4 códecs), WIA y GCZ dan el mismo encabezado lógico que la ISO (GZ2P01, disco 0, rev. 0).

Detector, antes de H32 → ahora: por contenido 3 164 de 6 264 líneas (51 %) → **463 de 6 172 (8 %)**;
por historial 5 228 líneas (3 710 sustantivas) → 1 105 (577) en 77 archivos: api 22/306, trivial
41/190, independiente 14/81, upstream 0, pendiente 0, problemas 0.

## Validación en hardware

H32-c1-b84a849829 queda **validado en hardware** (confirmación del usuario tras el análisis del log).
Se cierra únicamente H32 (procedencia); no se abre ni se planifica nada más aquí.

Probado por el usuario: controles (normal y Flip Grip), audio, mods estáticos, navegador, ajustes que
se conservan entre arranques, benchmark, y una ejecución con RVZ, zonas y dock/undock. Pruebas con el
mismo NRO (`0dce5ee8…5210`).

- **Benchmark** (F_SP122, 540p, ajustes idénticos): 29,52 FPS frente a 29,27 de H31-c1; p95 48,2 frente
  a 51,3 ms; Execute 31,9 frente a 32,3 ms; DrawRec 8,97 frente a 9,45 ms; memo de pipelines con 0
  misses. Igual o ligeramente mejor, dentro del ruido. El usuario ve 37-42 FPS en zonas normales y
  ~50 en las ligeras con el perfil de rendimiento.
- **Arranque desde RVZ** (`GZ2P01-test.rvz`, 946 507 908 B, el RVZ de zstd de `testkit/disc`), log
  `dusklight-20261001-140541.log`: `Loading DVD image: sdmc:/switch/dusklight/GZ2P01-test.rvz` y
  `Loaded game disc is GZ2P01`. La verificación sale «unknown» en Switch, que es lo esperado (no se
  calcula el hash).
- **Ejecución del RVZ** (Ordon y zonas vecinas: F_SP102, F_SP116, F_SP103, F_SP104, F_SP108), en
  perfil Quality: ~30 FPS tanto a 1080 filas acoplada como a 720 filas en portátil.
- **Dock/undock**: la resolución «auto» pasó de 540 (portátil con Flip Grip) a 1080 filas al acoplar
  (reloj de GPU 307 → 768 MHz, EMC 1331 → 1600 MHz), y a 720 filas al volver a portátil (307 / 1331
  MHz). Sin congelación; el juego siguió respondiendo.
- **Errores**: ninguno. El log tiene 215 líneas, todas INFO; no hay WARN, ERROR ni FATAL, ni mensajes
  de audio, mandos, Vulkan, memoria o lectura del disco; los mods se descargan limpiamente al salir.
- **Crash anterior** (`dusklight-20261001-130005.log`, `vkAllocateMemory … OUT_OF_HOST_MEMORY` a
  `res=6336 rows`): otra ejecución, por haber puesto el multiplicador de resolución en x12 por
  accidente. No se atribuye a H32.

Observaciones que no cambian el resultado y quedan registradas, sin más:
- La resolución «auto» acoplada es 1080 filas, igual que en H31 (`applyInternalResolutionScale` ya lo
  hacía); la nota de decisión de 900p en acoplado no está implementada en esta rama.
- El reporte del benchmark incluye una zona «Draw» sin llamadas.
- La interpolación predictiva puede quedarse limitada a 30 FPS hasta apagarla y encenderla; es de
  `game_clock.cpp`, que no cambió desde H31.
