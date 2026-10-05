# H28: Diagnostics, benchmark y reporte

Build privada de desarrollo. No se publica (ver `../../PROVENANCE.md`). Candidato físico: `H28-c1`.

## Qué trae H28-c1

1. Upstream Dusklight v2.0.3 fusionado en `aleks2` (merge `1e2574f7b7`, sin conflictos textuales).
2. `Ajustes > Aleks > Diagnostics (Aleks) > Run Benchmark`.
3. Reporte JSON versionado (`schema_version` 2 desde H29; H28-c1 escribió la versión 1).
4. SaltyNX: **no incluido** (ver más abajo).

## Benchmark

Pasos, en orden:

1. Se cierran los menús y se bloquea el input del juego. **Start cancela.**
2. Si no estás ya en `F_SP122` sala 8: warp a `F_SP122` (sala 8, punto 0, capa -1) y espera a que termine la carga.
3. 10 s de calentamiento.
4. Espera a que `queuedPipelines` esté a 0 durante 1 s seguido, con timeout de 90 s. Si vence se mide igualmente y el reporte lo marca (`run.pipeline_wait.timed_out`).
5. 60 s de medición (reloj de pared). Un frame es el intervalo entre inicios de iteración del bucle principal: incluye limitador y espera de pantalla.
6. Escritura del JSON, resumen visible (modal) y vuelta al sitio original.

No cambia ningún ajuste. Por tanto no hay ajustes que restaurar; lo que se restaura es el contexto:

- Mientras dura (y 3 s después de volver) `mDoMemCd_Ctrl_c::store()` no escribe nada y devuelve éxito, así que ni el guardado del menú ni el autoguardado ni un mod llegan a la tarjeta.
- Se guarda una copia en memoria del estado de guardado (`dSv_info_c`: savedata, memory, dan, restart, tmp, turnRestart) y se restaura al volver; Link se recoloca en su posición anterior si la sala coincide.
- Si la prueba se cancela o falla, se hace lo mismo.
- Si abres un menú durante la prueba se cancela. No se escribe reporte al cancelar.

### Ubicación del reporte

`<carpeta de datos>/benchmarks/bench-<AAAAMMDD-HHMMSS UTC>-<build id>.json`

En consola: `sdmc:/switch/dusklight/benchmarks/`. Se escribe como `*.json.tmp`, se hace flush/close y se renombra. Los errores (directorio, espacio libre, apertura, escritura, renombrado) se muestran en el resumen y en el log con su causa; la prueba no se pierde por ello (el resumen incluye todos los números).

### Contenido

`build` (id, tag, commit, `tree_dirty`, fecha de configuración), `run`, `device` (modo dock/handheld/flip_grip, API, adaptador, driver, clocks leídos de clkrst sin modificarlos), `resolution` (final, mínima y máxima observadas durante la ventana), `settings`, `drs` (activo, objetivo, transiciones en la ventana), `cache_state`, `frames` (media de FPS, 1 % low, mediana/p95/p99/máx de frametime, % > 33,3 ms y > 50 ms), `cpu_zones_ms` (por zona del profiler: media por frame, máximo, llamadas por frame), `gpu`, `gfx`, `companion`, `mods`.

Definiciones:

- FPS medio = frames / tiempo cubierto por los frames (no la media de FPS por frame).
- 1 % low = 1000 / media del frametime del 1 % de frames más lento.
- Percentiles por rango más cercano.
- Las zonas están anidadas (Execute y Audio dentro de Sim; Painter, Actors y DrawRec son hermanas dentro de Execute, y
  con interpolación el pintado pasa a la zona Draw del bucle principal): no suman el frame. (Corregido en H35; antes
  decía «Actors dentro de Painter».)
- `cache_state`: `compiling` si la cola de pipelines fue > 0 en algún frame de la ventana; `cold` si se crearon pipelines durante la prueba; `warm` si no. Aurora no permite distinguir un pipeline leído de la caché de disco de uno compilado: ambos cuentan como creados.
- `gpu.available` es `false`: no hay medición de GPU fiable en el bucle (Aurora solo alimenta zonas GPU a Tracy). No se inventa.

### Identidad de build

`platforms/switch/cmake/BuildIdentity.cmake` lee el commit al **configurar**: tras commitear hay que volver a configurar (`tools/configure.sh`). Etiqueta por defecto `H28-c1` (`-DALEKS2_BUILD_TAG=`). `tree_dirty` ignora archivos no seguidos, `platforms/switch/README.md` y `docs/`.

## Pruebas en PC

```
export PATH="/c/path/to/msys2/mingw64/bin:$PATH"
bash platforms/switch/tools/test-benchmark-report.sh
```

Cubre percentiles, resumen, JSON (también lo valida un parser real), escritura atómica y sus errores, marcas de tiempo y nombres. Lo acoplado al juego (máquina de estados, restauración, bloqueo de guardado) solo se puede probar en consola.

## SaltyNX: preparado, aplazado

No se ha incluido ningún código ni toggle. Razones concretas:

- **Protocolo no confirmado contra la fuente actual.** La única descripción disponible es la guía de Stevens (secundaria, 18/09/2026). El intento de leer `saltysd_proc/source/service.c` de SaltyNX falló (URL no encontrada) y no se verificó la estructura del bloque de 174 bytes ni las órdenes 6/7/0 contra el Status Monitor vigente. Implementar IPC con una firma equivocada contra un sysmodule puede tumbarlo.
- **Sesiones.** La conexión al puerto `SaltySD` consume una sesión de un proceso con tope bajo; la guía propone `svcSetResourceLimitLimitValue` (SVC 0x7E) o `smExit()`, ambos descartados por la condición de no usarlos sin demostrar seguridad con los hilos y servicios actuales. Sin ellos solo cabe el caso "hay sesión libre", que no se puede comprobar sin consola.
- **Licencia.** Los archivos de Stevens son GPL-3.0 y Dusklight es CC0: no se copian. Una implementación propia es posible desde el protocolo público una vez verificado.

Diseño previsto para cuando se retome (no implementado): hilo auxiliar de arranque que conecta, mapea, localiza/reserva el bloque, cierra la sesión y publica el puntero con release; después solo escrituras de memoria desde el hilo principal tras `aurora_end_frame()`, con `AuroraGetRenderSize()` y contadores por resolución durante el periodo; el toggle en Diagnostics, apagado por defecto, pone el puntero a nulo para detener la publicación; sin ReverseNX-RT ni sys-clk. Un no-op limpio si el puerto no existe (`0xF201`).

## Límites conocidos (requieren consola)

- Warp a `F_SP122` desde una partida real, detección de carga completa, restauración del estado en memoria y recolocación de Link.
- Que el bloqueo de `store()` cubra todos los caminos de guardado reales (menú, autoguardado).
- Que abrir el menú cancele de forma fiable y que Start cancele (el filtro de pad se aplica al final de `mDoCPd_c::read`).
- Valor real de `cache_state` en caché fría/caliente.
- Randomizer 1.0.6 no se reprobó contra la superficie ABI (el paquete no estaba disponible para la sonda); sí pasa `check-mod-abi.py` contra v2.0.3.
- Overlay `bench-status` (texto superior) y modal de resumen: aspecto sin ver.

## H29 (cambios sobre H28-c1)

- Retirado por completo el instanciado de dibujos heredado de souldbminer: `try_instance_draw`, el hash de vértices y su estado (`runSingle`, `runVertHash`, `runVertSize`, `lastPrim`), los contadores `drawsSeen`/`drawsMerged`, la variante `ShaderConfig::instanced` con su reescritura WGSL, el layout y bind group de uniformes instanciados, `DrawData::instanceRange`, `append_uniform_bytes`, `uniform_staging_*`, `fill_uniform_bytes`, la API `aurora_gx_*instancing*` y el ajuste `game.drawInstancing` (con su fila de UI). La fusión por concatenación de índices de upstream no se toca. `DrawData::instanceCount` se queda: lo usan líneas y puntos.
- `draw_prim` guarda la configuración de pipeline que ya derivó y `push_gx_draw` la reutiliza en vez de volver a llamar a `populate_pipeline_config`; `ShaderInfo` se carga directamente en su destino (`load_shader_info`) sin copia intermedia ni referencias a la tabla.
- El reporte (esquema 2) añade `gfx.pipeline_rebuilds_per_frame_avg`, `uniform_rebuilds_per_frame_avg` y `merge_blocked_{pipeline,textures,uniform_only,fmt}_per_frame_avg`, de contadores que Aurora ya tenía. Se quita `settings.draw_instancing`.
- Nota de caché: `ShaderConfig` recupera su campo de relleno original (`pad1` de 5 bits). Los pipelines instanciados que H27/H28 hubieran guardado en `pipeline_cache.db` quedan con ese bit activo en el relleno; son pocos (~1 fusión por frame) y se compilan como pipelines normales. La compatibilidad se comprobó desde `pipeline_cache.cpp`: tamaño y versión del `PipelineConfig` no cambian, la clave se recalcula sobre todos sus bytes al cargar y las configuraciones normales de H29 inicializan ese bit a cero, por lo que una entrada instanciada antigua no puede confundirse con una normal de H29.
- El heartbeat del log ahora imprime `draws/frame` y `merged/frame` del último frame.

## Estado de validación en hardware

- **H28-c1** (`H28-c1-86c0b0e768`): aceptado como base funcional el 2026-09-30/10-01. Línea base de rendimiento: dos JSON, F_SP122 sala 8, Flip Grip 733x540, clocks de portátil: 22,8–23,3 FPS, 1 % low 16,2–16,6, p95 55–58 ms, p99 59–60 ms.
- **H29-c1** (`H29-c1-04e1eaa087`, SHA-256 `2de0f2b8dc644547f469f7361ca42c00562697443db04eccb9d01c4744690e06`): **validado en hardware** el 2026-10-01 por el usuario. Sin errores visuales ni crashes; log sin errores ni advertencias. Una ejecución caliente en la misma escena y modo: 26,49 FPS, 1 % low 17,25, p95 53,8 ms, p99 56,8 ms, Execute 36,1 ms (H28: 41,5), DrawRec 16,7 ms (H28: 22,8). Es una sola ejecución; la mejora supera el ruido medido en H28 (2,4 %) pero no se repitió.
- Contadores nuevos de H29 (referencia para trabajo futuro, sin comparación con H28): `pipeline_rebuilds` 986/frame, `uniform_rebuilds` 1360/frame.
- **H30-c1** (`H30-c1-6f76027014`, SHA-256 `d84814caa9e71dae6b87b62c8b1b3c58772a31a6921224567d4b14975730ebca`): **validado en hardware** el 2026-10-01 por el usuario y **base de desarrollo actual**. Única diferencia de código/configuración frente a H29: Mesa/NVK 26.2.2 → 26.2.3 (`NVK: Mesa 26.2.3 (git-d4a00ea0ab)`, GM20B). Sin errores visuales, crashes, advertencias, device loss ni regresiones; benchmark, restauración y guardados correctos.
  - Una ejecución caliente en F_SP122 sala 8, Flip Grip 733x540, clocks stock de portátil: 26,69 FPS; 1 % low 17,54; p95/p99 53,6/56,0 ms; Execute 35,8 ms; DrawRec 16,3 ms; Painter 16,3 ms; pipeline/uniform rebuilds 986/1363 por frame. **Equivalente a H29 dentro del ruido (2,4 %); no es una mejora de rendimiento.** Caché caliente durante el benchmark; no se repite.
  - Primer arranque tras el cambio de driver: 584 pipelines en cola, a cero en ~270 s (el FPS del menú se mantuvo entre 22 y 31).
  - SDK usado: copia local `deps/mesa-naga-2623`, extraída de `mesa-26.2.3-switch-unified-horizon-sdk.zip` (SHA-256 `9b16d6dd07d01963d4c713281f3e6692cacca858195a143d7b2202afc2aeb346`). El `libvulkan.a` del ZIP tenía el índice de archivo incompleto (13 408 entradas, sin los miembros Rust `core`/`alloc`/`std`) y el enlace fallaba; se regeneró **solo el índice** con `aarch64-none-elf-ranlib -D` (SHA-256 antes `af39adc6…0100`, después `5d59d3b9…9892`; 1545 miembros idénticos y en el mismo orden). La copia **no** es idéntica byte a byte al ZIP; ver `deps/mesa-naga-2623/REPAIR-NOTES.txt`.
  - Procedencia: el PR #4 de `danfromtico/mesa-switch` (head `3a69711b8d…`) está contenido en el commit embebido `d4a00ea0ab`; no se puede demostrar que el `.a` salga byte a byte de él.
  - Reproducir la configuración de H30 requiere `-DALEKS2_STATIC_MODS="template_mod;flow_demo;rollandsprint" -DNAGA_VULKAN_ROOT=…/mesa-naga-2623/opt/devkitpro/portlibs/switch -DALEKS2_BUILD_TAG=H30-c1` en un directorio de build nuevo.

## Decisiones duraderas (2026-10-01)

- H30-c1 / Mesa 26.2.3 es la base de desarrollo actual. H29-c1 se conserva como fallback y referencia anterior.
- El SDK reparado con `ranlib` es válido para desarrollo y para esta prueba. **Antes de una V1 pública** hay que obtener un paquete corregido y reproducible o reconstruir Mesa desde las fuentes públicas.
- Los ~270 s de compilación de shaders tras un cambio de driver refuerzan la prioridad de implementar la pantalla «Preparando sombreadores» y la lista inicial de pipelines (`docs/switch-v1-roadmap.md`, Entregable B).
