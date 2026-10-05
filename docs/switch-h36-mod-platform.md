# H36 — Plataforma de mods en Switch

**Estado (2026-10-02): implementado y probado en host (H36-A), sin compilar para Switch ni probar en consola.**
Rama `claude/h36-mod-platform`, basada en `claude/h35-fifo-tour` `5e79c2f` (H35 con la corrección de enlace de
Aurora `845a511`). Uniform Reuse sigue en Off por defecto (decisión tomada con los resultados de H35-c1, §0); nada de
H35 cambia aquí.

El objetivo de H36 es que el candidato de Switch lleve mods útiles en lugar de las demostraciones: **Cosmetics, el
runtime de Luau y Roll and Sprint**, sin `template_mod` ni `flow_demo`, con una prueba real de que un mod de script
Luau se carga desde la SD. Randomizer queda fuera y auditado (§8).

## 0. Punto de partida: H35-c1 en consola

H35-c1 quedó **validado funcionalmente** en hardware: Prepare Shaders completó las seis zonas (Ordon punto 1 y Link's
House funcionan); Performance Tour, la restauración, las partidas y los logros funcionan; Verify dio 0 discrepancias
con `disabled_after_mismatch=false`.

**Decisión sobre Uniform Reuse: Off por defecto en H36, sin promover On.** En On solo se reutilizan
aproximadamente el 1,4–3 % de los bloques de uniforms y no bajan `fifo.worker_busy_ms_per_frame`, FPS, p95 ni p99; la
prueba adicional en Balanced 960×540 tampoco mostró beneficio. No se implementa otra variante de reutilización: el modo
queda como está en Aurora (Off/Verify/On, por defecto Off), útil como diagnóstico y nada más. H36 no toca nada de esto.

**Observación sin aislar, para investigar más adelante (no es trabajo de H36).** Balanced con interpolación mostró
≈ 25 ms/frame de FIFO en F_SP122 y se quedó en `stalled` el 87–95 % del tiempo en las zonas pesada y media, mientras
Link's House llegó a ≈ 53 FPS. No se ha comparado con Balanced con la interpolación desactivada en las mismas zonas, así
que no dice todavía si el coste viene de la interpolación, de la carga de esas zonas o de ambas. No se implementa ninguna
optimización a partir de esa comparación; el primer paso es medir las dos configuraciones con el Performance Tour en la
misma sesión (§11).

Una corrección pequeña del gobernador de interpolación, respaldada por esos mismos datos, va en H36 (§10).

## 1. Configuración propuesta de H36-c1

Un preset nuevo, `switch-devkitA64-release-h36`, hereda de `switch-devkitA64-release` y fija
`ALEKS2_STATIC_MODS` = `"cosmetics;luau_runtime;rollandsprint"`. Directorio de build propio
(`build/switch-devkitA64-release-h36`), así que no pisa la caché de H35.

```
git fetch origin claude/h36-mod-platform
git checkout claude/h36-mod-platform
git submodule update --init extern/aurora mods/cosmetics
git -C extern/aurora rev-parse HEAD     # 845a511c5b33588118554be2b410734f8c2cf2eb
git -C mods/cosmetics rev-parse HEAD    # 93971c38aa7286c59be7c3874a95e12d1a37036a
git status --short                      # vacío: BuildIdentity registra dirty=0
cmake --preset switch-devkitA64-release-h36 \
  -DNAGA_VULKAN_ROOT=<ruta>/mesa-naga-2623/opt/devkitpro/portlibs/switch \
  -DALEKS2_BUILD_TAG=H36-c1
cmake --build --preset switch-devkitA64-release-h36
```

Equivale a `cmake --preset switch-devkitA64-release -DALEKS2_STATIC_MODS="cosmetics;luau_runtime;rollandsprint" …`
en un directorio limpio. Opcional antes de compilar: `bash platforms/switch/tools/test-static-mods.sh` y
`bash platforms/switch/tools/test-luau-probe.sh` (§4).

Qué deja el build además del NRO:

| Ruta en `build/switch-devkitA64-release-h36/` | Qué es | A dónde va |
|---|---|---|
| `switch-mods/cosmetics/`, `switch-mods/luau_runtime/`, `switch-mods/roll_and_sprint/` | `mod.json` y `res/` de cada mod integrado | `sdmc:/switch/dusklight/mods/` (las tres carpetas) |
| `switch-test-mods/luau_probe.dusk` | la sonda Luau (diagnóstico, §3) | `sdmc:/switch/dusklight/mods/` solo para la prueba |
| `switch-mods/hook_targets.txt` | los 32 objetivos de hook de los mods | lo lee `hook_call_sites.py` (§5) |
| `generated/switch_builtin_symbols.cpp` | tabla de nombres resueltos en el enlace (§2.2) | compilado en el NRO |

En el log de configure deben aparecer `aleks2: mod '…' is compiled into the NRO` tres veces, `aleks2: Luau probe for
the card: … sha256 9c8a996c…` y `aleks2: 3 code mod(s) compiled into the NRO`; en el de build,
`static_mod_link: 3 mod(s), no shared template code, 3 name(s) for by-name resolution, 0 without a link name`.

## 2. Qué se implementó

### 2.1 Comprobación de conjunto: código de plantilla compartido entre mods

Problema encontrado en la auditoría. Las plantillas de las cabeceras del SDK usan por nombre los globales de cada mod:
`mods::HookImpl<Tag,…>::dispatch_pre` pasa `mod_ctx`, `mods::log::write<…>` llama a `svc_log`. `seal_mod.py` hace
locales esos globales en cada mod, pero las instancias de plantilla siguen siendo débiles y el enlazador conserva
**una** copia. Si dos mods instancian la misma (mismo objetivo de hook, mismo alias, la misma forma de llamada al log),
todos usan la del primero, con el contexto y los servicios del primero. No falla nada: hooks y log se atribuyen al mod
equivocado.

`platforms/switch/tools/static_mod_link.py` lee todos los objetos sellados a la vez (ELF64 a mano, sin dependencias) y
**detiene el build** si un símbolo débil definido por dos o más mods alcanza, directamente o a través de funciones
locales (incluidos los clones `.isra`/`.part` de GCC), estado propio de un mod: un global localizado, una sección
escribible fuera de grupos COMDAT o una exportación renombrada (`<mod>__mod_ctx`). Probado con objetos reales:

* el conjunto H36 (Cosmetics + runtime Luau + Roll and Sprint, AArch64 -O2) pasa;
* un mod que engancha `daAlink_c::procMoveInit` con el mismo alias que Roll and Sprint se rechaza, nombrando el
  trampolín y los dos `mod_ctx`;
* a -O0, `mods::log::write<int>` compartido entre Cosmetics y otro mod se rechaza (a -O2 GCC lo convierte en clones
  locales por mod y no hay riesgo, y la herramienta lo distingue).

### 2.2 Hooks por nombre en Switch: tabla resuelta en el enlace

Hallazgo bloqueante para Cosmetics. El build de Switch no tiene manifiesto de símbolos (en escritorio lo incrusta
`symgen` tras enlazar). Sin él no resuelven los hooks declarados por nombre (`DEFINE_HOOK_SYMBOL`) ni los hooks sobre
miembros virtuales que se resuelven por la vtable, y **Cosmetics devuelve error en `mod_initialize` en cuanto falla uno
de sus 28 hooks**: tres de ellos dependen de eso (`dMenu_DmapBg_c::buttonIconScreenInit` por nombre;
`dMeterHakusha_c::_create` y `COutFont_c::createPane`, virtuales).

Como todos los mods se enlazan en el mismo ejecutable, el enlazador puede dar esas direcciones. `static_mod_link.py`
lista lo que pide el metadata de cada mod sellado y genera `switch_builtin_symbols.cpp`, una tabla de referencias
**débiles**: si el enlace no tiene el símbolo, la entrada es nula y resuelve como «no encontrado», igual que una entrada
ausente del manifiesto; nunca rompe el enlace. `dusk::mods::manifest::resolve` recurre a ella en Switch
(`src/dusk/mods/manifest.cpp`, `src/dusk/switch_builtin_mods.hpp`) y al arrancar registra
`no symbol manifest on Switch; link-time symbol table: N name(s), M missing`, con una línea por nombre ausente.

Los nombres «de presentación» (`Clase::método`) no son nombres de enlace: `platforms/switch/static_mod_symbols.txt`
los mapea a su nombre mangled y a la cabecera del juego que los declara; la prueba compila una referencia a cada uno y
compara el símbolo que emite el compilador. Hoy tiene una entrada.

Límite: un símbolo **local** de un archivo del juego (`static`) no es visible para el enlazador; esos solo se pueden
resolver con un manifiesto posterior al enlace (H37, §8).

### 2.3 Selección y tabla integrada

`platforms/switch/cmake/StaticModSet.cmake` (nuevo, ejecutable en modo script) contiene la comprobación de
`ALEKS2_STATIC_MODS` y la generación de `switch_builtin_mods_table.cpp`, antes en línea en `StaticMods.cmake` y el
`CMakeLists.txt` raíz. Rechaza: entrada vacía, duplicada, con ruta, inexistente y submódulo sin checkout (con el comando
`git submodule update --init mods/<x>`); y en la tabla, ids duplicados, listas desparejas y nombres de target que no
pueden prefijar un símbolo C. La tabla generada es la misma que antes.

### 2.4 Sonda Luau (`dev.aleks2.luau_probe`)

`platforms/switch/test-mods/luau_probe/`: un mod de script (`"runtime": "dev.twilitrealm.luau@1.0"`) que se instala como
`.dusk` en `sdmc:/switch/dusklight/mods/`, por el flujo normal de descubrimiento. `platforms/switch/tools/make_dusk.py`
lo empaqueta de forma reproducible (SHA-256 `9c8a996c80070251e7f45cf1930d235e5ef70e39cd7a57dfbc2c7e8f61eb279f`) y el
configure lo deja en `switch-test-mods/` solo si el runtime Luau está en el build.

Cada línea empieza por `[h36-probe]`:

```
start: dev.aleks2.luau_probe v1.0.0 from <ruta del .dusk>
ok resource: probe.txt 132 bytes, first line 'H36 Luau probe resource, read from the package'
ok require: probe_util.luau loaded through require
ok compute: sum of squares 1..1000 = 333833500
ok notice: shown                      (y un aviso en pantalla: «Luau probe»)
success: 4 of 4 steps
update: 600 frames                    (unos 10–20 s después)
shutdown after N update(s)            (al desactivarlo o salir)
```

Si un paso falla escribe `FAIL <paso>: <motivo>`, `finished with N failed step(s)` y llama a `host.fail`, con lo que el
mod aparece como fallido en la ventana Mods. No registra ajustes, no toca partidas y no escribe archivos. Lo que prueba:
descubrimiento del paquete, el runtime integrado en el NRO ejecutando un script que viene de la SD (la ruta del `.dusk` y
el contenido de `probe.txt` salen en el log), `require` de un segundo script, el aviso de la UI y la desactivación sin
recompilar desde la ventana Mods.

### 2.5 Correcciones

* `mods/luau_runtime/src/runtime.cpp`: un `require` de un módulo inexistente leía un `std::optional` vacío para
  formar el mensaje de error (comportamiento indefinido en un simple error de script). Ahora nombra la ruta buscada.
  Lo encontró la prueba de la sonda.
* `platforms/switch/tools/hook_call_sites.py` (nuevo, solo lectura): tras enlazar, cuenta las llamadas directas a cada
  objetivo de hook en `dusklight.elf` y avisa de los que no tienen ninguna (el hook se instalaría sin interceptar nada,
  el problema que Roll and Sprint ya sufrió en H22–H30).

## 3. Estado real de cada pieza

| Pieza | Estado |
|---|---|
| Mecanismo estático (`StaticMods.cmake`, `seal_mod.py`, tabla, `switch_native_module.cpp`) | ya existía; se carga por id; un mod integrado necesita su carpeta (`mod.json`, `res/`) en `mods/` de la SD para aparecer y poder desactivarse |
| Descubrimiento | `scan_packages` sobre `sdmc:/switch/dusklight/mods` (`ConfigPath/mods`): carpetas con `mod.json` y archivos `.dusk`; un `.dusk` dañado se registra (`bad mod package …`) y se salta |
| Instalación desde el navegador | en Switch no hay vía local: la instalación por arrastrar solo existe en escritorio y la tienda en línea necesita red. **El flujo real en Switch es copiar a la SD.** Desinstalar/actualizar desde la UI: H37 |
| Activar/desactivar | ventana Mods; escribe `mod.<id escapado>.enabled` en `config.json` (acción del usuario); que un mod registre una variable de config no escribe nada (`config_register_var` no marca la config como modificada) |
| Recursos | siempre desde la SD (carpeta o `.dusk`) aunque el código esté en el NRO |
| `template_mod`, `flow_demo` | demostraciones; fuera del preset H36. Siguen en los builds de escritorio. Si sus carpetas están en la SD de builds anteriores, H36-c1 las lista como mods que no forman parte del build: quitarlas |
| Roll and Sprint | 4 hooks directos (no virtuales, sin nombre); sin `fmt`; sin solapamiento con Cosmetics |
| Cosmetics `93971c38` | `mod_initialize`/`mod_shutdown` correctos (quita hooks, desregistra texturas, vacía el mapa; reactivar reconstruye); 30 variables de config que solo se escriben cuando el usuario cambia un color en su ventana; `res/` vacío; 28 hooks, 3 dependían del manifiesto (§2.2). xxHash: Aurora lo declara primero con FetchContent y la declaración por git de Cosmetics se ignora (verificado con CMake 3.28: no se clona nada) |
| Runtime Luau | Luau 0.734 + LuaBridge3 twilit-v1, fijados por hash en su CMakeLists. Presupuesto de memoria por script de 64 MiB. **El límite de tiempo por script está desactivado** (`#if 0` en `runtime_activate`): un bucle infinito en un script cuelga el juego. En Horizon, `os.clock` y `TimeTrace` caen a `clock()` |

## 4. Probado en host

| Prueba | Resultado |
|---|---|
| `test-static-mods.sh` (mods compilados con `aarch64-linux-gnu-g++` 13.3 -O2 y sellados con `seal_mod.py`) | pasa |
| `test-interp-governor.sh` (8 pruebas H36 nuevas y las 9 anteriores; además con ASan/UBSan a mano) | pasa; una mutación que deja `discontinuity()` sin devolver el cooldown falla 5 comprobaciones |
| `test-luau-probe.sh` (ASan + UBSan + `_GLIBCXX_ASSERTIONS`, Luau 0.734, LuaBridge3 twilit-v1, miniz 3.0.2, fmt 12.1.0) | pasa, sin informes de ASan/UBSan ni fugas |
| Mutaciones: alias mangled erróneo; quitar la comprobación de ids duplicados | ambas hacen fallar `test-static-mods.sh` |
| Resto de `platforms/switch/tools/test-*.sh` | los 14 pasan (benchmark-report, config-override-audit, diagnostic-run, disc-header, disc-scan, interp-governor, perf-profile, perf-tour, pipeline-memo, profiler-ladder, remote-image-cache, resolution-policy, shader-prep, switch-patches) |
| Sintaxis de `manifest.cpp`, `switch_native_module.cpp` con `__SWITCH__` y `DUSK_CODE_MODS=1` | sin errores |
| `git diff --check` | limpio |

`test-static-mods.sh` cubre: preset H36 (presencia de los tres, ausencia de `template_mod`, `flow_demo`, `randomizer`);
selección válida y cinco inválidas; tabla integrada desde los `mod.json` y `add_mod` reales y tres tablas inválidas;
`static_mod_link.py` sobre el conjunto (sin código compartido, 3 nombres, 32 objetivos: 4 + 28) y sobre combinaciones
que debe rechazar; objeto truncado, archivo de alias mal formado y objeto ausente; el archivo de alias contra la
cabecera; las dos tablas generadas compiladas, enlazadas y leídas con la búsqueda real (presentes → dirección, ausentes →
nulo); que los nombres de la tabla coinciden con lo que produce el sellado (`ld -r` sin exportaciones indefinidas); y
`hook_call_sites.py` sobre un programa enlazado, en host y en AArch64.

`test-luau-probe.sh` usa el código real de descubrimiento, manifiesto, lector zip, `borealis_io_switch.cpp`, servicio de
recursos y el mod runtime Luau completo; log, host, UI y config son dobles que registran. Cubre: descubrimiento de la sonda
y de las tres carpetas de mods integrados; `.dusk` truncado y `mod.json` inválido (se registran y se saltan); tarjeta
vacía y carpeta inexistente; la sonda completa (cada línea, un aviso, 600 updates, desactivar, que no reciba más updates,
reactivar); cero llamadas a config y a `data_dir`, y ningún archivo creado, cambiado o borrado; errores de sintaxis, de
ejecución, sin `main.luau`, `require` de módulo inexistente y recurso ausente, cada uno con su motivo; y que el runtime
sigue funcionando después.

## 5. Pendiente de compilación (H36-c1)

1. Configure: que `find_package(xxHash)` **no** encuentre un paquete en la máquina de build. Si lo encontrara, Aurora no
   declararía FetchContent `xxhash`, Cosmetics clonaría xxHash por git y el alias `xxHash::xxhash` chocaría. El log de
   configure dice `aurora: Fetching xxhash` en el caso bueno.
2. Que Luau 0.734 compile y enlace con devkitA64/newlib (ya hay un `-include climits` para Luau.VM en `StaticMods.cmake`)
   y que `clock()` esté disponible en libnx (lo usan `os.clock` y `TimeTrace`).
3. La línea de `static_mod_link` del §1 y que el enlace del NRO no tenga símbolos indefinidos.
4. Llamadas directas a los objetivos de hook, tras compilar:
   ```
   python3 platforms/switch/tools/hook_call_sites.py \
     --objdump aarch64-none-elf-objdump --nm aarch64-none-elf-nm \
     build/switch-devkitA64-release-h36/switch-mods/hook_targets.txt \
     build/switch-devkitA64-release-h36/dusklight.elf
   ```
   Lo esperable es que los 4 de Roll and Sprint tengan llamadas (las opciones `-fno-ipa-*`/`-fno-inline` de H22–H30 están
   para eso). De los 28 de Cosmetics, cualquiera con `NO DIRECT CALLER` no hará nada en consola; no impide arrancar.
   `dMenu_DmapBg_c::buttonIconScreenInit` tiene una sola llamada en su propio archivo y es candidato a quedar inlineado.
5. Tamaño: medido con `aarch64-linux-gnu-g++` -O2 (no devkitA64), `.text` de los objetos sellados: Cosmetics ≈ 180 KB,
   runtime Luau ≈ 188 KB, Roll and Sprint ≈ 5 KB; las bibliotecas Luau completas (VM, compilador, AST, bytecode) ≈ 1,39 MB
   como máximo. En total, menos de ≈ 1,8 MB de código más que un NRO sin mods; anotar el tamaño real del NRO.

## 6. Pendiente de hardware: protocolo corto (≈ 17 min)

Antes: NRO H36-c1 en `sdmc:/switch/dusklight/`; copiar las tres carpetas de `switch-mods/` y
`switch-test-mods/luau_probe.dusk` a `sdmc:/switch/dusklight/mods/`; quitar de ahí `template_mod`/`flow_demo` si
quedaron; copia de `config.json` y de las partidas, con su hash. Uniform Reuse en Off (por defecto; no cambiarlo).

1. **Arranque (2 min).** En el log (`sdmc:/switch/dusklight/cache/logs/`): `Built-in mod '…'` para los tres;
   `link-time symbol table: 3 name(s), 0 missing`; la línea de hooks de Switch (svc 0x73) sin rechazo; ningún
   `hook target … did not resolve`. Aparece el aviso «Luau probe».
2. **Ventana Mods (1 min).** Cuatro entradas activas: Cosmetics, Luau runtime, Roll and Sprint, Luau Probe (H36).
3. **Sonda (3 min).** En el log, las líneas del §2.4 hasta `success: 4 of 4 steps` y luego `update: 600 frames`.
   Desactivar la sonda en la ventana Mods → `shutdown after N update(s)`. Reactivarla → `start` y `success` otra vez.
4. **Cosmetics (5 min).** Abrir su ventana desde el panel Mods; cambiar el color de la gorra de la túnica de héroe y de
   los corazones; comprobar ambos en juego. Devolverlos a «Default». Desactivar Cosmetics → colores originales;
   reactivarlo. Anotar si el selector de color se maneja con el mando.
5. **Roll and Sprint (2 min).** Rodar y mantener para esprintar, como en builds anteriores.
6. **Gobernador (2 min, §10).** Con Balanced e interpolación: ir a una zona pesada (F_SP122) hasta que el log diga
   `interp: stalled`, y viajar a Link's House. En el log, `interp: discontinuity while stalled` si la carga detuvo el
   bucle ≥ 250 ms, y `interp: resumed` en unos 5–6 s; sin esa línea, en 20 s como mucho. Anotar cuál de los dos casos.
7. **Cierre (2 min).** Salir del juego. `config.json` solo debe diferir en las claves `mod.….enabled` tocadas y en las
   de Cosmetics que se cambiaron; las partidas no deben cambiar salvo que se haya guardado. Copiar el log.

## 7. Criterios para mantener o retirar Luau y Cosmetics

**Runtime Luau: se mantiene** si compila y enlaza sin parches nuevos, la sonda da `success: 4 of 4 steps` y
`shutdown` al desactivarla, y ni el arranque ni el benchmark empeoran más allá del ruido frente a las cifras de H35-c1
en Off. **Se retira** del preset (sin tocar el resto) si no enlaza con newlib/libnx, si la sonda falla en un paso que en host pasa, si cuelga o se
cierra el juego, o si su coste fijo es medible con el runtime cargado y sin scripts. El límite de tiempo desactivado no
es motivo de retirada para H36-c1 (la sonda no tiene bucles abiertos), pero sí condición para admitir scripts de terceros
en H37.

**Cosmetics: se mantiene** si `mod_initialize` termina (log sin `failed to add … hook`), los cambios de color se ven en
juego y desaparecen al desactivarlo, y no hay escrituras fuera de sus claves de `config.json`. **Se retira** si no
inicializa (algún hook que no resuelve o no instala), si cuelga o corrompe gráficos, o si `hook_call_sites.py` muestra
que los hooks de sus funciones principales (túnica, corazones, espadas) no tienen llamadas. Hooks secundarios sin
llamadas se documentan, no lo retiran.

## 8. Randomizer: auditoría (decisión para H36/H37)

Commit del submódulo `1f24e81` (v1.0.6). **No entra en H36.** Bloqueos y costes:

* **12 de sus 17 hooks por nombre apuntan a funciones `static` de archivos del juego** (`daObj_Gb_Create`,
  `daAlink_searchBouDoor`, `b_bq_end`, `demo_camera_end`, `action_check`, `daNpc_Ks_Execute`, `dStage_playerInit`,
  `daKytag08_Execute`, `daE_MD_Create`, `daDitem_Execute`, `lure_heart`, `uki_catch`). Son locales: la tabla de enlace de
  §2.2 no las puede ver. Y su `initialize` devuelve `MOD_ERROR` en cuanto falla un hook. En Switch **no inicializaría**
  sin un manifiesto de símbolos posterior al enlace (symgen o equivalente sobre `dusklight.elf` antes de `elf2nro`).
* **Comparte objetivo con Cosmetics**: `dMenu_Ring_c::_create`. Con el SDK actual, `static_mod_link.py` rechazaría
  Randomizer + Cosmetics en el mismo build (§2.1). Hace falta que el SDK ate `HookImpl` a cada mod (por ejemplo, con el
  mod en el `Tag`), lo que es un cambio de ABI del SDK.
* 92 hooks en total (75 directos, 17 por nombre); `hook_call_sites.py` tendría que validarlos todos.
* Dependencias descargadas en configure **por etiqueta git, sin hash**: yaml-cpp `yaml-cpp-0.9.0`, base64pp
  `v0.2.0-rc0` (release candidate), battery-embed `TwilitRealm/embed` `twilit-v2`. Habría que fijarlas por commit y
  revisarlas.
* Tamaño: ≈ 25 000 líneas de C++ (generador incluido); embebe `generator/data` en el binario (≈ 2,0 MB sin las pruebas)
  y lleva ≈ 2,0 MB de `res/` en la SD.
* Escribe: semillas (`seed.dat`) y presets YAML bajo `data_dir`, y un blob en cada partida (servicio de save,
  `set_blob`) además de observar guardados. Lee `dComIfGs` y cambia flags de evento: depende de la progresión.
* Hilos: generación de semilla y precarga en hilos `std::thread` desacoplados (`detach`), más un hilo de barra de
  progreso. Generar una semilla es CPU y memoria intensivo; en la consola no está medido.

Recomendación: H37 como muy pronto, y solo con (1) manifiesto posterior al enlace en Switch, (2) SDK con hooks atados
al mod, (3) dependencias fijadas por hash, (4) una sesión propia que mida la generación de semillas.

## 9. Riesgos

* **Dependencias.** Nada nuevo descargado en el build: Luau, LuaBridge3 (por hash), xxHash (vía Aurora) y fmt ya
  estaban fijados. Para las pruebas en host se usaron clones por etiqueta de luau `0.734` (`3fc82b1071`), LuaBridge3
  `twilit-v1` (`06ef10a0a7`), miniz 3.0.2 (zip de la release, SHA-256 `ada38db0…c5`) y el compilador
  `aarch64-linux-gnu-g++` de Ubuntu; nada de eso entra en el repositorio ni en el NRO. El archivo de GitHub de luau no
  se pudo descargar aquí, así que el hash fijado en `mods/luau_runtime/CMakeLists.txt` no se ha recomprobado.
* **Memoria.** Cada mod de script tiene su VM con un tope de 64 MiB; la sonda usa una fracción mínima. Cosmetics guarda
  copias recoloreadas de texturas mientras está activo. Sin medir en consola.
* **Tiempo.** Sin límite de tiempo por script (ver §3). Roll and Sprint y Cosmetics corren en hooks del bucle del juego.
* **Hooks.** Requieren que la consola permita svc 0x73 (`probe_self_patch`); si no, los hooks se rechazan limpiamente y
  Cosmetics/Roll and Sprint no inicializan, pero el juego arranca.

## 10. Gobernador de interpolación: cooldown acotado y cargas

`src/dusk/interp_governor.hpp`, `src/dusk/game_clock.cpp`. Evidencia (H35-c1, portátil, Balanced 960×540, Fancy,
companion apagado; ventanas de 30 s del Performance Tour):

| Zona | Uniform Reuse | `stalled` | `resume_blocked_by_cooldown_frames` | Recuperaciones |
|---|---|---|---|---|
| F_SP122 | Off | 95,2 % | 769 | 1, con una recaída |
| Ordon | Off | 100 % | 901 | ninguna en 30 s |
| F_SP122 | On | 90,7 % | 767 | 2, con dos recaídas |
| Ordon | On | 87,3 % | 785 | una, a los 36,31 s |

Cientos de frames por zona cumplían ya la regla de trabajo para volver (trabajo EMA < 30 ms durante 30 frames) y
esperaban solo al cooldown, que crecía 5 → 10 → 20 → 40 → 60 s y seguía castigando después de una carga. En juego, el
jugador veía 30 FPS fijos durante demasiado tiempo tras una parte pesada en escenas que pueden dar 37–40 FPS.

| | Antes (H33–H35) | H36 |
|---|---|---|
| Entrada en `stalled` | intervalo EMA > ~35 ms, 30 frames | igual |
| Salida | trabajo EMA < 30 ms, 30 frames, cooldown cumplido | igual |
| Cooldown inicial | 5 s | igual |
| Tras recaer (< 15 s de una vuelta) | ×2 hasta 60 s | ×2 hasta **20 s** (5, 10, 20) |
| Vuelta a 5 s | 30 s interpolando sin recaer | **15 s** |
| Carga, pausa o cambio de tasa | solo se descartaban las EMA | `discontinuity()`: se mantiene el estado; votos y EMA a cero; cooldown a 5 s contado desde la discontinuidad; una vuelta anterior deja de contar como recaída; los contadores no cambian |

Qué cuenta como discontinuidad: `game_clock::reset()` (fin de una pausa, cambio de tasa de simulación) y un intervalo
nativo ≥ 250 ms (`kMaxSampleSeconds`, el mismo límite con el que ya se descartaba la muestra). Un cambio de escala de
tiempo sigue siendo solo un descarte de estimaciones. Cuando ocurre estando en `stalled` se registra
`interp: discontinuity while stalled (gap=… ms); cooldown back to 5 s`. **No está comprobado que una carga de zona en
Switch detenga el bucle 250 ms**; si no lo hace, de la corrección quedan el tope y el reinicio a los 15 s (paso 6 del §6).

Tiempo máximo hasta volver a intentar la interpolación con la escena ya ligera:

* **tras una recaída:** 20 s desde la entrada en `stalled` (antes 60 s). Los votos se acumulan durante el cooldown, así
  que el intento llega en la primera muestra después de él; si la escena se aligera más tarde, 30 frames (≈ 1 s a
  33,3 ms) después;
* **tras una carga** detectada: 5 s desde la discontinuidad, o 30 frames de muestras nuevas (≈ 1 s) si eso es más; en
  ningún caso en el primer frame, y nunca antes de esos 5 s;
* **sin recaídas:** 5 s, como antes.

Límite de oscilación (`interp_governor_test.cpp`): la escena patológica que solo es pesada interpolando, 20 min, da
5,75 transiciones por minuto (con el tope de 60 s, 2,25 medidas con la misma simulación); con una carga cada 30 s, 7,8 por minuto. Cada intento
fallido cuesta ≈ 1,2 s de frames lentos (H33), así que en F_SP122 con interpolación, el peor caso medido, el coste pasa
de ≈ 2 % a ≈ 6 % del tiempo; a cambio, una escena que de verdad se aligera vuelve en ≤ 20 s en lugar de ≤ 60 s. No se
cambian umbrales, dwell ni el control principal (Predictive Interpolation), ni nada de FIFO, Uniform Reuse o
interpolación. Los informes del Performance Tour de H36 no son directamente comparables con los de H35 en
`stalled_pct_of_window` ni en `resume_blocked_by_cooldown_frames`: es justo lo que cambia.

## 11. H37 y fuera de alcance

Manifiesto de símbolos posterior al enlace para Switch; hooks del SDK atados a cada mod; dependencias y orden de carga
generales; instalar/actualizar/desinstalar desde la UI en Switch; límite de tiempo de scripts; Randomizer. Fuera de H36
por decisión: cargador dinámico, compatibilidad general, controles tipo Wii, cross-save, red, cambios de rendimiento de
H35, Uniform Reuse en On u otra variante de reutilización (descartadas con los datos de H35-c1, §0).

Investigación futura de rendimiento, fuera de H36: aislar el coste de Balanced con interpolación en F_SP122 y en las
zonas medias (§0) midiendo con y sin interpolación en la misma sesión antes de proponer ningún cambio.

Hallazgo de paso, sin corregir (código upstream): `file_exists`/`directory_exists` de `src/dusk/mods/svc/resource.cpp`
devuelven `MOD_INVALID_ARGUMENT` desde una función `bool`, así que una ruta no segura cuenta como «existe». El runtime
Luau no llega a ese caso (normaliza antes).
