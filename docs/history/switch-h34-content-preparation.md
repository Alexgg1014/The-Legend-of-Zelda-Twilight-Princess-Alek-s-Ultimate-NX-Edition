# H34: selector de disco, navegador de mods sin tirones y Prepare Shaders

**Estado: H34-c2 (`H34-c2-7f2c328c9f`) se compiló y probó en hardware; sus tres correcciones funcionan (ver
«Resultado de H34-c2 en hardware», al final). Queda un fallo de Prepare Shaders en Ordon, con la causa identificada,
que se corrige dentro de H35 y no en un H34-c3.** H34-c1 se había probado antes en hardware con dos fallos y una
protección pendiente, corregidos para H34-c2 («Correcciones H34-c2»). El resto de este documento describe H34-c1 tal
como se escribió.

## Base

| Repositorio | Rama | Commit |
|---|---|---|
| principal | `claude/h34-content-preparation` (nueva) | base `7350859524b7df8f52ea84d2c9267d215f94bd54` (cierre de H33, código H33 `52076ba037`) |
| `aurora-aleks-nx` | `claude/h33-aurora-window-fit` | `0020cbf6365c171e1f25988567e1d3a51168b7bc`, **sin cambios** (gitlink intacto) |

Ni `main`, ni `.gitmodules`, ni Borealis, ni Aurora, ni Mesa, ni los perfiles o el gobernador de H33 se tocaron.

## Alcance

Un solo hito con tres bloques, todos completados:

1. **A. Selector seguro de disco** (Switch).
2. **B. Tirones del navegador de mods**: medido por código, corregido y con telemetría para confirmarlo en consola.
3. **C. Prepare Shaders**: recorrido automático y cancelable de seis zonas.

Fuera de alcance: cargador dinámico `.so`, compatibilidad completa con ALBW/Dawnlight/Essentials, deko3d, cambios de
Mesa/backend, tuning del gobernador, optimizaciones FIFO (solo auditoría: [switch-h35-fifo-plan.md](switch-h35-fifo-plan.md)).

---

## A. Selector seguro de disco

### Lo que había (auditoría)

- `backend.isoPath` se lee una vez al arrancar (`m_Do_main.cpp`): si no valida (`iso::inspect`, solo cabecera) se
  borra y se abre el prelanzador; después se llama a `aurora_dvd_open` **una sola vez**. Cambiar de imagen con el
  juego en marcha no es seguro: el cambio solo puede aplicarse **al reiniciar** (Switch no admite reinicio del
  proceso, `SupportsProcessRestart = false`).
- En Switch no hay selector de archivos (`borealis_file_select_switch.cpp` responde siempre «cancelado»), así que
  **Ajustes › Disc Image** y **Play** sin disco no hacían nada. Lo único que existía era la autoselección del
  primer `.iso/.gcm/.rvz/.wia/.gcz` de `sdmc:/switch/dusklight/` cuando no hay disco configurado
  (`find_switch_disc_image`, validado en H32; **se conserva sin cambios**).
- La validación de H32 (`disc_header.cpp`) ya abre la imagen con el mismo lector de contenedores que usa el juego y
  lee los primeros 0x20 bytes **lógicos**; no hashea ni descomprime la imagen.

### Diseño

- `platforms/switch/source/disc_scan.cpp` (+ `switch/disc_scan.hpp`): escáner puro, sin UI ni ajustes.
  - Solo `sdmc:/switch/dusklight/` y `sdmc:/switch/dusklight/discs/` (esta segunda es opcional: si no existe no
    pasa nada), **un nivel, nunca recursivo**.
  - Filtra por extensión antes de abrir nada (`.iso .gcm .rvz .wia .gcz`, sin distinguir mayúsculas; se ignoran
    los `._*` de macOS). Como máximo 256 entradas por carpeta y 64 imágenes listadas.
  - Identifica cada imagen con `read_logical_header` sobre un flujo con **presupuesto de bytes (16 MiB)**: nunca lee
    ni descomprime la imagen completa; si un contenedor necesitara más, se lista como dañado. `bytesRead` informa lo
    leído (en la prueba: ≤ 4 KiB para una ISO de 1,4 GiB).
  - La ruta configurada siempre aparece, marcada, aunque esté fuera de las carpetas o ya no exista. **El escáner
    nunca la reescribe.**
- `iso_validate`: `classify_header()` da el mismo veredicto que `inspect()` para una cabecera ya leída (refactor
  mínimo: `inspect_switch` = leer + clasificar).
- UI (`ui/prelaunch.cpp`, solo `__SWITCH__`): `DiscPickerModal`. El escaneo corre en un worker de borealis
  (`borealis::spawn`), la lambda solo posee copias; si el modal se cierra, la tarea se cancela y su resultado se
  descarta. Cada fila muestra nombre · formato · veredicto · carpeta (y «current» para la configurada). Solo las
  imágenes del juego en una versión admitida se pueden elegir.
- Al elegir: se vuelve a leer la cabecera de **esa** imagen (puede haber cambiado la tarjeta) y se guarda por el
  camino existente (`apply_valid_disc_result` → `backend.isoPath` + `config::save()`). Si ya hay un disco abierto,
  el texto del modal y un aviso dicen que se usará **al volver a abrir Dusklight**; el aviso de «Pending restart»
  del prelanzador y la marca de modificado en Ajustes ya existían. Nunca cambia de disco sin acción del usuario.
- `config.json`: sin claves nuevas; compatible en ambos sentidos.

### Formatos

| Formato | Lector | Evidencia |
|---|---|---|
| ISO / GCM (sin comprimir) | `disc_container` (raw) | hardware (estado del proyecto; los registros de H32/H33 de este repositorio citan el RVZ) |
| RVZ | `disc_container` | **hardware**: `GZ2P01-test.rvz` (zstd) arrancó en H32 y H33. RVZ lzma/bzip2/none: solo pruebas host (`testkit`) |
| WIA | `disc_container` | solo pruebas host; **no probado en Switch** — etiquetado «experimental» en el selector |
| GCZ | `disc_container` | solo pruebas host; **no probado en Switch** — etiquetado «experimental» |
| CISO, WBFS, NFS, TGC | detectados, rechazados | se listan como «format not supported» si tienen extensión de imagen; con su propia extensión ni se abren |

---

## B. Navegador de mods

### Flujo auditado

| Paso | Hilo (antes de H34) | Evidencia |
|---|---|---|
| Catálogo: petición HTTP | worker de borealis (pool de 8) | `borealis/src/http.cpp` (`submit_task`) |
| Catálogo: parse JSON | **worker** (`fetch_page(...).map(finish_request)`; `Task::map` corre en el hilo que completa) | `mods/catalog.cpp:450`, `borealis/task.hpp` |
| Reconstrucción del DOM | principal, una vez por página (o por cambio de generación del cargador) + `UpdateDocument()` | `mod_browser.cpp` `ModBrowser::update` |
| Carga de textura de un card | principal (RmlUi → `LoadTexture` → proveedor) | `aurora/lib/rmlui/WebGPURenderInterface.cpp:516` |
| Placeholder | principal: textura transparente de hasta 256×256 (hasta 256 KiB copiados y subidos) | `remote_texture_provider.cpp` |
| Descarga de miniatura | worker | `http::start` |
| **Decode PNG + premultiplicado** | **principal**, dentro de `update_remote_texture_provider`, hasta **4 en el mismo frame** | `finish_request` → `decode_png` |
| Publicación | principal: `Rml::ReleaseTexture` de **todas** las terminadas a la vez → RmlUi recrea y sube todas en el render siguiente | `finish_request` |
| Orden de peticiones | orden de un `unordered_map` (arbitrario) | `update_remote_texture_provider` |
| Caché de disco | **no existe**; solo memoria (64 imágenes / 64 MiB) | — |
| `mod://` (iconos de mods instalados) | principal y síncrono (lectura + decode + mipmaps); afecta a la ventana de mods, no al navegador online | `mod_texture_provider.cpp` |

Hallazgo de RmlUi (fork `encounter/RmlUi@f00a0fee`, `Element.cpp:275-285`): un subárbol fuera de la región de recorte
no se renderiza y **la carga de texturas de decoradores se difiere hasta que es visible**. El proveedor solo ve lo
visible.

### Causa demostrada (por código)

El trabajo que podía bloquear un frame era, por orden de coste:

1. **Decodificación PNG en el hilo principal**, varias en el mismo frame (un banner de ≥ 640 px de ancho; la fuente
   elegida es la primera *al menos* tan ancha, puede ser mayor).
2. **Publicación en ráfaga**: todas las imágenes terminadas se liberaban a RmlUi en el mismo frame y se recreaban y
   subían juntas (copia completa RGBA a `m_pendingUpload` + creación de textura por cada una).
3. Sin prioridad visible ni precarga, y sin límite al tamaño decodificado.

**No está medido en consola**: la magnitud real (ms por decode en un A57) la dará la telemetría de H34-c1. La red y
el parse del catálogo ya estaban fuera del hilo principal.

### Optimizaciones aplicadas

- **Decode, premultiplicado y escalado en el worker** que completa la descarga (`Task::map`). El worker solo posee
  sus búferes y cadenas; nunca ve la caché.
- `remote_image_cache` (nuevo, sin RmlUi ni borealis, probado en host), un solo hilo (el de UI):
  - **Prioridad**: Visible > Adelantada (una fila) > Normal (páginas de detalle, iconos de la cola) > Diferida
    (cards fuera de alcance: no se piden). Las pistas las calcula `ModBrowser` con la geometría de RmlUi solo
    cuando cambian el scroll, la altura o la cobertura. Como RmlUi no pide texturas fuera de pantalla, las pistas
    **crean** la entrada de la fila adelantada.
  - Mientras otra página (detalle de un mod) cubre el navegador, sus cards pasan a Diferida: el detalle no compite.
  - **Deduplicación** por fuente: nunca dos descargas ni dos decodes de la misma imagen; una recarga de RmlUi es un
    acierto en memoria.
  - **Concurrencia** 4 (como antes), sin un hilo por imagen (pool compartido de borealis).
  - **Publicación presupuestada**: 1 MiB y 4 imágenes por frame (al menos una), visibles primero.
  - **Memoria**: 64 imágenes / 64 MiB (valores previos), 256 entradas rastreadas como máximo; expulsión LRU que
    nunca toca lo visible o adelantado y prefiere lo ya entregado a RmlUi; una imagen mayor que todo el presupuesto
    se rechaza.
  - **Tamaño por miniatura**: los cards nunca se decodifican mayores que el ancho que piden (640 banner, 256 icono
    como arte, 128 icono): escalado por promedio de área en el worker. Detalle y capturas mantienen la fuente
    completa (sin cambio visual ahí).
  - **Tickets**: cada petición lleva uno; un resultado de una página abandonada, de un navegador cerrado o tras
    `unregister` es *stale* y se descarta. Al cerrar el navegador sus peticiones pendientes se cancelan y sus
    placeholders se liberan (si otra vista los sigue mostrando, RmlUi los vuelve a pedir).
- Las dos columnas, el scroll, el foco y el diseño **no cambian**. La lista y los placeholders aparecen igual que
  antes; ninguna parte del layout espera a una imagen.

### Lo que permanece en el hilo principal (necesariamente)

- La llamada del proveedor desde RmlUi (búsqueda en la caché y devolver un `span`).
- La creación de la textura WebGPU y la copia de los píxeles a `m_pendingUpload`, y su subida: las hace Aurora
  dentro de `LoadTexture`/`GenerateTexture` en el hilo que renderiza RmlUi. **No hay evidencia de que esa ruta sea
  thread-safe**, así que no se mueve. H34 solo decide *cuándo* (presupuesto por frame).
- `Rml::ReleaseTexture`, la reconstrucción del DOM y el layout.
- No se modificó Borealis ni Aurora.

### Telemetría

Una línea `INFO` al cerrar el navegador (`dusk::ui::modBrowser`), sin registro por frame: peticiones de catálogo (ms
última/máx., incluye red y parse), reconstrucciones (ms máx./total), actualizaciones de pistas; imágenes solicitadas,
descargadas (KiB), decodificadas, escaladas, reutilizadas, publicadas, canceladas, *stale*, fallos de red y de decode,
rechazadas, expulsadas, olvidadas; máximo de pendientes, de cola y de publicación; memoria actual/límite/pico; ms de
red y de decode medidos **en el worker** (media/máx.); y en el principal: ms por búsqueda del proveedor, ms máx. de la
actualización, frame máx. y **frame máx. inmediatamente después de publicar** (el frame en que RmlUi recrea las
texturas: medida indirecta del coste de creación/subida, que no puede medirse sin tocar Aurora).

---

## C. Prepare Shaders

### Qué es y qué no

**Ajustes › Aleks › Diagnostics › Prepare Shaders**, separado de *Run Benchmark* y *Run Profile Matrix*. Visita
zonas para que sus pipelines se creen ahora; **no mide FPS**. No cambia ningún ajuste.

### Arquitectura

Un tercer tipo de ejecución dentro de `benchmark.cpp` (fases `PrepLoading`, `PrepRender` y la `Restoring` común),
para **reutilizar sin duplicar** sus protecciones: guardado bloqueado desde el primer frame
(`mDoMemCd_Ctrl_c::store` no escribe), copia en memoria de `dSv_info_c` antes del primer warp y restauración al
volver (también si el recorrido acaba en el lugar de origen tras visitar otros), recolocación de Link, entrada
bloqueada (Start cancela), un menú abierto cancela, `begin_restore()` en toda salida, 3 s de gracia antes de
rehabilitar los guardados. La parte sin juego (zonas, regla de parada, progreso, informe) está en
`src/dusk/shader_prep.cpp` y se prueba en host.

### Zonas y evidencia

Todas salen de la tabla de warp del propio repositorio (`src/dusk/map_loader_definitions.h`); la prueba host
verifica cada etapa/sala/punto contra ese archivo. Capa −1 (la que elige el progreso de la partida).

| # | Zona | Etapa / sala / punto | Cobertura | Evidencia adicional |
|---|---|---|---|---|
| 1 | Outside Castle Town - West | F_SP122 / 8 / 0 | exterior pesado | escena del benchmark, warp validado en hardware H28–H33 |
| 2 | Castle Town | F_SP116 / 0 / 0 | población | cargada en hardware (log H32) |
| 3 | Ordon Village | F_SP103 / 0 / 0 | pueblo, **zona media** | cargada en hardware (log H32) |
| 4 | Link's House | R_SP01 / 4 / 0 | interior | solo tabla de warp |
| 5 | South Faron Woods | F_SP108 / 0 / 0 | bosque, vegetación y niebla | cargada en hardware (log H32) |
| 6 | Forest Temple | D_MN05 / 0 / 0 | mazmorra | solo tabla de warp |

`F_SP102` (pantalla de título) y `F_SP112` (fondo de selección de partida) quedan fuera, igual que en el benchmark.
Las categorías «interior» y «mazmorra» **no tienen evidencia de hardware**: es lo primero que debe mirar la prueba.
Ninguna zona permite juzgar 45/60 FPS; Ordon Village es una candidata para medir después la zona media, no en H34.

### Duración y terminación anticipada

Por zona: carga (límite 120 s; si no carga, la zona se marca *failed* y se pasa a la siguiente), dibujo mínimo 15 s,
y luego termina cuando la cola lleva 3 s vacía y **o bien** no se creó ningún pipeline en la zona (*warm*), **o bien**
no se crea ninguno nuevo desde hace 10 s (*drained*); como máximo 150 s (*timed_out*: el resto sigue compilando en
segundo plano). Si al terminar hay una cinemática en curso, se le dan hasta 30 s antes del siguiente warp.

- Caché fría: ≤ ~17 min (6 × (carga + ≤ 150 s)); objetivo del usuario 10–20 min.
- Caché caliente: ~2–3 min (6 × (carga + 15 s)).
- Si Link sale de la zona por sí solo (salida, evento), lo dibujado cuenta y la zona se marca *skipped*.
- La cámara no se mueve (no se inyecta entrada): un warp cubre lo que se ve desde la entrada. Mover la cámara
  queda como mejora si el informe muestra poca cobertura.

### Cancelación y restauración

- Start, abrir un menú o un fallo → la zona actual se marca *cancelled*/*failed*, se pide volver al lugar de origen
  (aunque el warp estuviera pendiente) y se restaura el estado en memoria. Si no se puede volver en 120 s, los
  guardados se rehabilitan igualmente (comportamiento existente del benchmark) y el informe lo dice.
- `config.json` no se escribe: no hay overrides que persistir; `config::save_count()` (nuevo, contador interno)
  permite al informe **confirmarlo** (`config_saves_during_run`).

### Informe y telemetría

`<datos>/benchmarks/shaders-<UTC>-<build>.json`, esquema `dusklight-aleks-shader-prep` v1 (nuevo; **el esquema del
benchmark sigue en 3**). Se escribe cuando el jugador ya volvió, para confirmar: zonas previstas/completadas/omitidas/
fallidas/canceladas/no alcanzadas; por zona: etapa a la que llegó, carga y dibujo (s), cola inicial/máxima/final,
pipelines creados, motivo de fin; global: pipelines conocidos al inicio, cola inicial/máxima/final, creados, duración,
resultado (*completed*/*cancelled*/*failed*) y motivo; `returned_to_original_place`, `in_memory_save_state_restored`,
`saves_reenabled`, `config_json_written`. Una línea `INFO` por zona y un resumen final en el log; el estado en
pantalla (barra superior existente) muestra `Preparing shaders NN% - zone i/6: nombre (categoría) - N new, M queued`.

### Cachés: qué es cada una

| Caché | Qué guarda | Dónde | Invalidación / portabilidad |
|---|---|---|---|
| `pipeline_cache.db` (Aurora) | **recetas** de pipeline (config GX, RmlUi, clear) con el primer frame en que se usaron | `sdmc:/switch/dusklight/cache/` | `aurora_schema` = 1 y versión de config por tipo (GX = 13); al arrancar se encolan todas en segundo plano (`rebuild_pipeline_cache`) |
| `dawn_cache.db` (Aurora/Dawn) | **blobs** compilados por Dawn/NVK | `sdmc:/switch/dusklight/cache/` | esquema 2; claves de Dawn; las semillas iniciales se ignoran si la identidad del driver no coincide; se poda al vaciarse la cola tras el arranque |
| Caché del driver/Mesa | la de NVK, si la hay, por debajo de Dawn | — | ligada a la versión exacta de Mesa (26.2.3) |
| Memo de H31 | `PipelineRef`+`ShaderInfo` por config, **solo RAM**, hilo FIFO | — | se vacía con `pipeline_generation()` (init/shutdown/rebuild) |
| Imágenes del navegador | miniaturas decodificadas, **solo RAM** | — | se pierde al cerrar la UI; sin caché en disco |

Prepare Shaders **no** crea un formato nuevo: llena las dos bases de Aurora con su propio código. No se distribuye
ninguna caché en el NRO, no se descarga ninguna y no se comparte entre consolas. Una caché no es portable entre
builds con distinto `GXPipelineConfigVersion`, versiones de Mesa/Dawn o consolas con otro driver; Aurora ya lo
gestiona con sus esquemas y la identidad del driver.

`tools/make-shader-seed.py` (citado en el roadmap antiguo) **no se reconstruye**: solo sirve para distribuir una
semilla dentro del NRO, que H34 descarta expresamente; Prepare Shaders no lo necesita.

---

## Cambios de ABI y de formatos persistentes

- **ABI pública de mods: sin cambios.** No se tocó `sdk/include`, `mod_abi_surface.txt` ni `mod_abi_pinned.txt`;
  `UserSettings` no cambia. Lo añadido es interno (`config::save_count`, `iso::classify_header`, una sobrecarga de
  `remote_image_source`, miembros de `ModBrowser`). `check-mod-abi.py` sigue sin existir en este repositorio.
- `config.json`: sin claves nuevas.
- Informe del benchmark: esquema 3, sin cambios. Nuevo archivo de informe de Prepare Shaders (esquema propio v1).
- Fuente de texturas remotas: formato interno `…#size=WxH` con `,max=N` opcional (H34-c2; H34-c1 usaba `&max=N`,
  ver «Correcciones H34-c2»); no se persiste.

## Riesgos

- **Prepare Shaders en partidas tempranas**: cargar Castle Town, Faron o el Templo del Bosque antes de llegar a ellos
  puede disparar eventos o variantes (Twilight) de esa etapa. Mitigaciones: entrada bloqueada, espera de evento con
  límite, zona omitida si Link sale, restauración completa del estado en memoria y guardado bloqueado. El caso peor
  heredado del benchmark (no poder volver en 120 s) deja al jugador en otra zona con el estado restaurado y los
  guardados activos.
- Un warp con un evento en curso pasados 30 s: el mismo mecanismo que usa la restauración del benchmark.
- Los pipelines que el arranque aún esté compilando en segundo plano cuentan dentro de la zona en curso.
- Navegador: el tamaño de los placeholders (≤ 256 KiB por fuente) no cambió; si la telemetría muestra coste en el
  frame de carga inicial, es el siguiente candidato.
- `classify_header` registra un `WARN` por cada archivo de imagen inválido encontrado al escanear (igual que
  `inspect`).

## Pruebas host (todas pasan en la nube)

Nuevas: `test-disc-scan.sh` (extensiones, formatos por contenido, directorio vacío/inexistente, archivo vacío,
truncado, cabecera inválida, contenedor rechazado, límites de entradas y de candidatos, no recursivo, espacios y no
ASCII, ruta configurada dentro/fuera/borrada sin reescribirla, barra final, ISO dispersa de 1,4 GiB leída ≤ 4 KiB,
GCZ con presupuesto insuficiente); `test-remote-image-cache.sh` (con ASan/UBSan: formato de fuente, escalado,
deduplicación, prioridad, límite de concurrencia, cancelación, tickets y resultados tardíos, fallos de red/decode,
aciertos, presupuesto de publicación, memoria y expulsión, límite de entradas, cierre con trabajo pendiente, precarga
de la fila adelantada, la lista no espera a las imágenes); `test-shader-prep.sh` (zonas contra la tabla de warp, regla
de parada, duración, progreso, informe JSON válido en completed/cancelled/failed, resumen).
Existentes, sin cambios y pasando: benchmark-report, config-override-audit, disc-header, interp-governor, perf-profile,
pipeline-memo, profiler-ladder, resolution-policy, switch-patches.

Además, comprobación de sintaxis (`-fsyntax-only`, host y `-D__SWITCH__` con un `switch.h` mínimo) de
`remote_texture_provider.cpp`, `mod_browser.cpp`, `prelaunch.cpp`, `iso_validate.cpp`, `benchmark.cpp`,
`shader_prep.cpp`, `config.cpp` y `settings.cpp` contra RmlUi `f00a0fee`, fmt 12.1.0 y las cabeceras del repositorio.
**No es una compilación de Switch** (ni devkitA64 ni libnx reales). `git diff --check` limpio; sin binarios, secretos
ni rutas locales nuevas (los scripts de disco conservan el `ALEKS2_MINGW` por defecto que ya usaba
`test-disc-header.sh`). El medidor de procedencia (`provenance-residue.py`) no puede medir en la nube (falta la referencia
de su fork); todo el código nuevo es propio.

## Lo que solo puede probarse en Switch

Que compile y enlace con devkitA64; el modal del selector y su escaneo en la SD real; el flujo de reinicio; el decode
en worker sin tirones en la red real de la consola; que RmlUi recargue las texturas publicadas; los warps de las seis
zonas (sobre todo interior y mazmorra) con la partida del usuario; la restauración tras el recorrido y tras cancelar;
el tiempo real del recorrido frío y caliente; que la segunda ejecución sea corta.

## Plan mínimo para una única prueba de H34-c1 (≈ 60–75 min)

Compilar como en H33 (`-DALEKS2_BUILD_TAG=H34-c1`, configurar **después** del checkout, árbol limpio). Antes:
copia de `config.json` y de la carpeta `cache/`; anotar el hash de `config.json`.

1. **Arranque y disco (10 min)**: arrancar desde el RVZ. Ajustes › Disc Image: aparece la lista; la imagen actual
   marcada «current». Copiar a `sdmc:/switch/dusklight/` un archivo `.iso` falso (p. ej. renombrar un `.txt`): debe
   salir «not a disc image» y no poder elegirse. Si hay ISO y RVZ, elegir la otra: aviso de reinicio; cerrar y abrir
   Dusklight: arranca con la nueva (`Loading DVD image:`); volver a la original. Opcional: una WIA/GCZ si se tiene.
2. **Navegador (10 min)**: abrir Browse online mods, desplazar arriba/abajo, abrir un detalle y volver, cambiar de
   página, cerrar. Mirar a simple vista los tirones y que las dos columnas siguen igual. En el log, la línea
   `Mod browser closed: …`: anotar `decode_ms(worker)`, `update_ms_max`, `frame_after_publish_ms_max` y
   `frame_ms_max`.
3. **Prepare Shaders en frío (≤ 20 min)**: con la caché de la prueba copiada aparte (o tras borrar `cache/` **solo si
   el usuario lo decide**; si no, en el estado actual), desde una zona normal: lanzar, comprobar que el texto superior
   avanza zona a zona, dejar terminar. Al volver: misma posición, resumen, `shaders-*.json`.
4. **Prepare Shaders en caliente (≤ 5 min)**: repetir; debe ser mucho más corto (zonas *warm*).
5. **Cancelación (5 min)**: lanzar y pulsar Start durante la carga de la zona 2 y, en otra ejecución, durante el
   dibujo de la zona 4: vuelve, informe *cancelled*, guardar después funciona.
6. **Regresión (10 min)**: Run Benchmark una vez (Quality, mismo modo que H33) y una partida normal con guardado.

Aceptación: compila; sin crash, ERROR, OOM ni pérdida de dispositivo; el selector nunca cambia el disco sin elegirlo y
el cambio se aplica al reiniciar; navegador sin tirones evidentes y con dos columnas; Prepare Shaders termina o se
cancela siempre volviendo al lugar de origen, con `saves_reenabled: true` y `config_json_written: false`; hash de
`config.json` idéntico salvo por el cambio de disco elegido a propósito; benchmark dentro de ±3 % de H33.

Motivos para no aceptar H34-c1 (y diagnosticar antes de cualquier cambio): un warp que deja al jugador fuera de su
lugar, guardado escrito durante el recorrido, `config.json` modificado sin acción del usuario, crash al abrir/cerrar
el navegador o al publicar imágenes, el selector reescribiendo `isoPath` solo.

---

## Correcciones H34-c2

Tres correcciones sobre `44fdd17`, sin ampliar el alcance: mismas zonas, mismo selector, mismos límites del navegador,
sin tocar Aurora, Borealis, Mesa ni submódulos.

### 1. Parpadeo de miniaturas

**Causa (confirmada con el RmlUi real y la telemetría de consola):** la fuente de una tarjeta era
`…#size=1280x720&max=640`. `mod_browser.cpp` la pasa por `escape()` antes de escribirla en la propiedad `decorator`, y
RmlUi no deshace el escape en los valores de propiedad, así que el proveedor recibía `…&amp;max=640`. Las pistas usaban
la forma sin escapar: cada miniatura tenía **dos entradas**. La que RmlUi mostraba nunca quedaba fijada como visible,
se expulsaba (límite de 64 entradas listas), volvía al placeholder y se descargaba otra vez. Además, el parser de c1 no
reconocía `&amp;max=`: esa entrada tenía placeholder 1×1 y se decodificaba sin límite. Telemetría de consola: 270
descargas para 237 fuentes, 206 expulsiones, `stale=0`, hasta 4 descargas siempre activas.

**Forma canónica:** `…#size=WxH,max=N` (`remote_image::kMaxMarker`). `escape()` no transforma la coma, y RmlUi solo
separa decoradores por comas **fuera** de paréntesis (`PropertyParserDecorator.cpp`, `ExpandString(…, ',', '(', ')')`).
`make_source` solo emite esta forma. `parse_source` acepta también `&max=` y `&amp;max=` únicamente por robustez, y
`canonical_source()` las normaliza a la nueva. `set_image` devuelve, como clave de pista, el texto exacto que recibe el
decorador (la fuente ya escapada, idéntica a la canónica), así que pistas, `lookup`, inicio de descarga, publicación y
`ReleaseTexture` usan una sola clave. No se quitó `escape()`, no se cambió la concurrencia, los límites, la expulsión
ni los workers, y el decode sigue en el worker.

**Simulación** (`remote_image_flicker_sim.hpp`: RmlUi falso con base de texturas por clave literal, 48 tarjetas en
dos columnas, 96 imágenes, recorrido hacia abajo y hacia arriba y 600 frames quieto, con los límites reales):

| | descargas | expulsiones | publicadas | descargas con la lista quieta | imágenes visibles que desaparecen | entradas por miniatura |
|---|---|---|---|---|---|---|
| H34-c1 (código de `44fdd17`) | 1352 | 1284 | 1216 | 800 | 1092 | 2 |
| H34-c2 (clave canónica) | 128 | 64 | 16 | 0 | 0 | 1 |

En c2, las 32 descargas de más sobre las 96 imágenes son reentradas legítimas al volver hacia arriba: 96 imágenes no
caben en 64 entradas. Cada una se pide una sola vez y ninguna en bucle.

### 2. `config_json_written: true` falso

**Causa:** `configSavesAtStart` se leía antes de `ui::close_all_documents()`. Cerrar Settings ejecuta su propio
`config::save()` síncrono (`SettingsWindow::hide`), y el informe lo atribuía al recorrido.

**Orden nuevo** (`src/dusk/diagnostic_run.hpp`, usado por los tres tipos de ejecución):
1. bloquear los guardados de partida;
2. suspender los logros;
3. cerrar la UI (aquí termina el guardado de Settings);
4. leer `config::save_count()` como línea base.

Después vienen el primer warp y el recorrido. `config::save()` y `config.json` no cambian, y una escritura real
posterior se sigue detectando.

### 3. Logros propios durante diagnósticos

Dusklight usa su propio `AchievementSystem` local, sin RetroAchievements ni ningún servicio remoto. Se evalúa en cada
frame, recibe señales del juego y guarda en `achievements.json`. Los warps de un diagnóstico podían cumplir
condiciones y escribirlas para siempre.

**Diseño:**
- `src/dusk/achievement_signals.hpp` (sin cabeceras del juego) contiene las señales del sistema y un contador de
  suspensión anidable.
- `AchievementSystem::suspend()`/`resume()` lo usan, y `ScopedAchievementSuspension` lo mantiene con RAII.

**Mientras está suspendido:**
- `signal()` descarta la señal al llegar; nada se acumula.
- `tick()` no carga, no evalúa, no cambia progreso y no guarda.
- La primera suspensión limpia lo pendiente. Solo el último `resume()` la levanta, y vuelve a limpiar.
- Un `resume()` sin pareja se ignora.
- Fuera de una suspensión, `tick()` hace exactamente lo de antes, incluida la conservación de señales mientras el
  juego aún no ha arrancado.

**Alcance:** Run Benchmark, Run Profile Matrix y Prepare Shaders comparten la misma infraestructura de warps
(`benchmark.cpp`), así que los tres quedan cubiertos con la misma guarda.

**Ciclo de vida:**
- La suspensión empieza antes del primer warp y la guarda el `Run` en curso.
- Al final, `end_run()` (por donde pasan todas las salidas: completado, cancelado en carga o en dibujo, zona que no
  carga, salida anticipada, no poder volver) sigue este orden:
  1. el jugador y la escena ya están restaurados;
  2. se rehabilitan los guardados;
  3. se escribe el informe;
  4. se reanudan los logros, lo último.
- Si el `Run` se destruye sin pasar por `end_run`, la guarda se libera con él.
- No se toca ninguna preferencia del usuario, y los toasts no son el mecanismo.

### Pruebas host de c2

- `test-remote-image-cache.sh` (ASan/UBSan) añade:
  - la clave emitida igual a la que RmlUi entrega (réplica de `escape()`);
  - `maxDimension` = 640;
  - placeholder 256×144 con aspecto 16:9;
  - `ReleaseTexture` con la misma clave;
  - una sola entrada por miniatura;
  - ni `&max=` ni `&amp;max=` en la forma canónica;
  - normalización de las formas antiguas;
  - la simulación anterior, que exige 0 descargas con la lista quieta y 0 imágenes visibles desaparecidas.
- `test-diagnostic-run.sh` (nuevo, ASan/UBSan) cubre:
  - el orden de inicio y la línea base después del guardado de Settings;
  - la detección de una escritura posterior;
  - `config_json_written: false` sin cambios, también al cancelar;
  - los guardados rehabilitados y los logros reanudados en cada salida;
  - logros: una condición que desbloquearía no desbloquea, el progreso no sube, `achievements.json` no se escribe,
    las señales no cruzan la reanudación y una señal normal funciona después;
  - anidamiento, `resume()` sin pareja, RAII y una ruta de excepción.

La máquina de estados de `benchmark.cpp` en sí sigue necesitando la consola.

### Solo comprobable en Switch

- Que no queden miniaturas parpadeando con el catálogo real.
- Que el log diga `Achievements suspended…` / `Achievements resumed…` alrededor de cada ejecución, y que
  `achievements.json` no cambie (hash) tras un recorrido.
- `config_json_written: false` tras abrir Prepare Shaders desde Settings.

---

## Resultado de H34-c2 en hardware

Datos del usuario (2026-10-02), una sesión de prueba:

| | |
|---|---|
| Build ID | `H34-c2-7f2c328c9f` |
| Commit | `7f2c328c9f432eccf6312869331795adce0dee9a` |
| SHA-256 del NRO | `cdfcdf39bafcb4e5a15219e4e85d37b25a5c27ff2bd966b3315132b00cce4237` |

Las tres correcciones funcionaron:

- el parpadeo de miniaturas del navegador desapareció;
- `config_json_written` quedó en `false`;
- los logros se suspendieron y reanudaron correctamente; partidas y escena original restauradas;
- sin errores de Vulkan, memoria, dispositivo, audio ni mandos.

Benchmark en portátil, **companion desactivado**:

| Perfil | Resolución | FPS | DrawRec | 1 % low |
|---|---|---|---|---|
| Quality | 1280×720 | 29,43 | ≈ 10,7–11,3 ms | ≈ 20–21 |
| Balanced | 960×540 | 29,29 | ≈ 10,7–11,3 ms | ≈ 20–21 |
| Performance | 960×540 | 29,31 | ≈ 10,7–11,3 ms | ≈ 20–21 |

Bajar de 720p a 540p no mejoró nada: la escena parece limitada por CPU/FIFO, no por la GPU. Es la premisa de H35
([switch-h35-fifo-plan.md](switch-h35-fifo-plan.md)). Las cifras no se comparan con ejecuciones con el companion
encendido.

### Fallo de Prepare Shaders en Ordon

Prepare Shaders entraba en Ordon Village por `F_SP103` sala 0 **punto 0**. En una partida nueva ese punto corresponde a
«Ordon Ranch -> Ordon Village» y dispara la secuencia de las cabras: el juego sale a `F_SP00`, la zona queda `skipped`
(el comportamiento previsto cuando Link sale de la zona) y después Link's House puede no cargar. La tabla de entradas
del Randomizer (`TwilitRealm/dusklight-randomizer@1f24e81`, `generator/data/entrance_shuffle_data.yaml`; etapa 43 =
`F_SP103`) lo confirma: en la sala 0, el punto 0 es «Ordon Ranch Village Pathway -> Ordon Village» y el punto 1
«Outside Links House -> Ordon Village».

La prueba host de H34 no lo podía ver: solo comprobaba que el punto estuviera en la tabla de warp del repositorio, y
los dos lo están. **Se corrige dentro de H35** (punto 1 y una prueba que exige etapa, sala, punto y capa exactos), sin
crear un H34-c3 ni otra compilación de H34. El Performance Tour de H35 usa la misma entrada.
