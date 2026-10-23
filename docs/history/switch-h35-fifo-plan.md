# H35: plan para el cuello de botella FIFO/Painter (auditoría de solo lectura)

**Estado (2026-10-02): implementado en la nube como H35-c1, sin compilar ni probar en consola.** La auditoría de
abajo se escribió durante H34 y se conserva como estaba, salvo dos correcciones marcadas; lo implementado, la
auditoría completa de `fill_uniform` y el protocolo de prueba están en «Implementación H35 (H35-c1)», al final. Base
de lectura de la auditoría original: rama `claude/h34-content-preparation` (Aurora `0020cbf`). Etiquetas: **[H]** hecho leído en el código (con `archivo:línea`,
rutas de Aurora bajo `extern/aurora/`), **[I]** inferencia a confirmar con la instrumentación propuesta. Las
estimaciones en ms son para un Cortex-A57 a ~1 GHz y **no son mediciones**.

## Evidencia de partida

| | H30 | H31 | H32 | H33 (acoplado, F_SP122) |
|---|---|---|---|---|
| FPS | 26,69 | 29,27 | 29,52 | 29,0–29,6 |
| Execute (ms) | 35,8 | 32,3 | 31,9 | 31,6–32,1 |
| DrawRec (ms) | 16,3 | 9,5 | 8,97 | 10,1–11,2 |
| Painter (ms) | 16,3 | 19,5 | — | — |
| Draws/frame | 1450 | 1458 | — | ~1480–1510 |

Performance trabaja ~1,8 ms menos por frame que Balanced (1,0 ms menos en DrawRec). Rebuilds por frame: pipeline ~986,
uniform ~1360 (H29/H30).

## Lo que de verdad mide cada zona

Todas las zonas `DUSK_PROFSCOPE` están en el **hilo principal** y miden tiempo de pared (`src/dusk/profiler.hpp:196-205`).
Ninguna está en Aurora. [H]

```
Events | Begin | VIStub | UI | Sim{ Execute{ Painter | Actors | DrawRec } Audio } | Present
```

- **Execute**: `fapGm_Execute` → `fpcM_Management`. **Painter, Actors y DrawRec son hermanas**
  (`src/f_pc/f_pc_manager.cpp:84-116`). [H] Hay dos sitios que dicen otra cosa, «Actors dentro de Painter»: el texto
  de `docs/history/switch-h28-diagnostics.md` y un comentario de `src/dusk/benchmark.cpp` (lista de zonas). **Corregidos en
  H35.** Matiz que faltaba [H]: Painter solo corre dentro de Execute cuando simulación y presentación no están
  separadas (`IF_DUSK_BLOCK(!separatePresentation)`); con interpolación (o escala de tiempo) el bucle principal pinta
  en la zona **Draw** (`m_Do_main.cpp`) y ni Painter ni TickWait corren.
- **Painter** = `mDoGph_Painter` (`src/m_Do/m_Do_graphic.cpp`). Recorre las listas J3D/2D y **escribe** comandos GX
  en el búfer FIFO. **Incluye la espera del tick**: `beginRender` → `JFWDisplay::beginRender` → `waitForTick` →
  zona `TickWait` (`libs/JSystem/src/JFramework/JFWDisplay.cpp:383`), cuando la interpolación está apagada. [H] Con la
  captura del HUD de doble pantalla activa, también drena la FIFO (`src/dusk/dualscreen.cpp`). [H]
- **DrawRec** = `fpcDw_Handler` (`src/f_pc/f_pc_draw.cpp:38-50`). Hace el recorrido de dibujo de los actores y después
  `cAPIGph_AfterOfDraw` → `JFWDisplay::endFrame` → `JFWDrawDoneAlarm` → `GXDrawDone` → **`fifo::drain()`**
  (`JFWDisplay.cpp:337-345, 581-590`; `lib/dolphin/gx/GXManage.cpp:263-267`). Ocurre cada frame (2 XFB). [H]
  **DrawRec = recorrido + espera bloqueante del hilo principal hasta que el hilo FIFO traduce todo lo del frame.**
- **Begin** = `aurora_begin_frame`: espera de un slot de frame y del staging (sondeos con `sleep_for` de 100 µs y de
  1 ms, `lib/gfx/frame.cpp:581-622`) + ImGui. [H]
- **Present** = `aurora_end_frame`: un segundo drain (≈0, porque DrawRec acaba de drenar), grabación del final del
  frame, RmlUi y encolado del EndFrame. [H]

**Relectura de H31** [I, muy probable]: el memo redujo el trabajo del **hilo FIFO**, así que la espera dentro de DrawRec
bajó 6,8 ms. El frame terminaba antes y el limitador del tick (dentro de Painter) dormía más: los +3,3 ms de Painter
son espera, no trabajo. Se confirma con la fila `TickWait` de cualquier JSON de H31+ (Painter_net = Painter − TickWait).

## Hilos y sincronización

| Hilo | Núcleo | Qué hace |
|---|---|---|
| Principal (juego) | 0 (`platforms/switch/source/switch_startup.cpp`) | produce el flujo GX |
| «Aurora FIFO processor» | **solo 1** (`lib/thread.cpp:262`, `Affinity::Fifo` = `0b010`) | `process()`, command processor; graba Commands y escribe vértices, uniforms y storage en el staging; encola pasadas al render worker |
| Render worker | 2 | codifica pasadas, submit, present (vsync) |
| Compilación de pipelines | 2, prioridad baja | — |
| **Callback de audio y todos los OSThread del juego** | **1, el de la FIFO** | `src/dusk/audio/DuskAudioSystem.cpp:168` y `src/dusk/OSThread.cpp:134`: `svcSetThreadCoreMask(…, 1, 0x2)` [H] |

El último punto contradice el diseño escrito en `lib/thread.cpp:246-249` («whatever shares its core is taken out of the
frame»). [H]

- **Búfer FIFO** (`lib/gx/fifo.cpp`): contiguo, 64 KB iniciales, crece duplicando y sin límite. Se vacía en cada
  `drain`, así que el productor nunca espera por espacio. El consumidor tiene `sBufferMutex` durante cada `process()`;
  el productor solo lo toma al crecer, al drenar y al limpiar. [H]
- **Publish** (`fetch_add` + `notify_all` en cada uno): en cada `GXCallDisplayList`, cada 16 `GXEnd`, `GXCopyTex`,
  `GXSetDrawDone`… [H]
- **Drain** (espera atómica sobre `sProcessed`, `fifo.cpp:252-263`): `GXDrawDone` (cada frame, en DrawRec),
  `end_frame`, `AuroraGXSync` (mods), `create_pass`/`resolve_pass`/`push_custom_draw`. [H]
- **Render worker**: cola acotada de 256 con mutex y 2 variables de condición; 3 slots de frame y 5 búferes de staging
  (`lib/gfx/frame.hpp:13-18`). [H]
- No hay ninguna espera activa escrita a mano en la ruta FIFO. [H] `atomic::wait` de libstdc++ sin futex en Horizon
  probablemente cuesta una llamada al sistema por publish cuando el worker duerme. [I]

## Ruta caliente por draw (hilo FIFO)

- `settle_pipeline_dirty`: con `DirtyPipeline`, rehace `populate_pipeline_config` (2772 B) y un `memcmp` **en cada
  intento de draw** (`lib/gx/command_processor.cpp:616-632`). [H]
- `refresh_pipeline` (~986/frame): hash xxh3 de 2772 B + `memcmp` en el memo; **siempre fuerza `DirtyUniform`**
  (`command_processor.cpp:474`). [H]
- `refresh_uniform` (~1360/frame): `fill_uniform` reescribe **todas** las matrices (10 pos, 10 tex, 10 nrm y las 20
  ptTex si se usa alguna) y las luces, ~1,7–3,4 KB [I]; luego `UniformRepeat` hashea el bloque entero y no lo empuja si
  se repite (`shader_info.cpp:374-534`). [H]
- `refresh_bind_groups`: un `sampler_ref` por textura (xxh3 + mutex) y `bind_group_ref` (xxh3 + mutex). [H]
- `push_verts` copia los vértices de la FIFO al staging (segunda copia). `push_command` copia ~176–192 B por Command
  en un `std::vector`. [H]
- Asignaciones: el FramePacket se recrea con `= {}` cada frame; el vector de la primera pasada no reserva memoria y
  crece duplicando; las pasadas de continuación hacen `reserve(2048)`. [H]
- Render worker por draw: `get_pipeline` con el mutex global `g_pipelineMutex`; `SetIndexBuffer`, `SetBindGroup(1)` y
  `SetImmediates` sin filtrar. Solo se filtran texturas, constante de mezcla y pipeline. [H]
- Ya existe filtrado de registros redundantes (caches BP/CP y comparaciones XF, `lib/gx/regs.cpp`). [H]

## Qué falta medir

Hoy no se distingue: tiempo ocupado/ocioso del hilo FIFO; **espera de drain** (y en qué sitio); Painter_net (no se
publica); tiempo ocupado del render worker; esperas de slot y staging (solo llegan a Tracy, que está apagado); CPU del
callback de audio; aciertos de `UniformRepeat`; publishes y despertares por frame.

## Oportunidades priorizadas

Premisa [I]: como el hilo principal espera en DrawRec a que acabe el hilo FIFO, cada ms ahorrado en el hilo FIFO resta
un ms de DrawRec mientras quede espera.

**1. Reutilizar el bloque de uniforms cuando solo cambió el pipeline**
- Evidencia: `command_processor.cpp:474` y `shader_info.cpp:501-534`.
- Hipótesis: el bloque depende solo del estado GX y de unos campos de `ShaderInfo` (tamaño, lineMode, loadsTevReg,
  iluminación, canales y KColors muestreados, ptTexMtx, niebla, matrices indirectas, texturas muestreadas). Si el
  estado no está sucio y esos campos coinciden con los anteriores, se puede reutilizar el rango sin rellenar ni
  hashear.
- Beneficio: 0,3–1,5 ms [I].
- Riesgo: bajo-medio (uniforms viejos si falta un campo).
- Archivos: `lib/gx/command_processor.cpp`, `lib/gx/shader_info.{cpp,hpp}`.
- Telemetría: rellenos, aciertos de repetición y reutilizaciones.
- Prueba host: modo de verificación que calcula los dos caminos y exige bytes iguales.
- Prueba en Switch: F_SP122, A/B en el mismo NRO, más una pasada visual.
- Aceptación: `fifo_busy` −0,5 ms y 0 discrepancias.
- Revertir: cualquier diferencia visual o discrepancia.

**2. Sacar el audio y los OSThread del núcleo de la FIFO**
- Evidencia: las dos llamadas `svcSetThreadCoreMask(…,1,0x2)` arriba y `lib/thread.cpp:246-270`.
- Hipótesis: compiten con el hilo FIFO y alargan la espera.
- Beneficio: 0–2 ms, igual a su CPU dentro de la ventana FIFO; desconocido sin medir.
- Riesgo: bajo en código y medio en audio (cortes si el núcleo 2 se satura). Mantener el audio por encima de la
  compilación.
- Telemetría: ms de callback de audio por segundo, cortes de audio, `fifo_busy` y `drain_wait`.
- Prueba en Switch: F_SP122 y una escena con mucho audio.
- Aceptación: `drain_wait` o `work_ms` −0,7 ms, sin cortes.
- Revertir: cualquier chasquido o un resultado dentro del ruido (±2,4 %).

**3. Abaratar la derivación del config de pipeline**
- Evidencia: `populate_pipeline_config` en cada probe (`:616-632`) y xxh3+`memcmp` en el memo.
- Hipótesis: memoizar por una clave compacta de los registros que la alimentan, o un MRU de las últimas N.
- Beneficio: 0,5–1,5 ms [I].
- Riesgo: medio (que la clave quede completa).
- Prueba host: `test-pipeline-memo.sh` ampliado, config byte a byte.
- Aceptación: −0,5 ms con hit rate 1,0.
- Revertir: cualquier discrepancia.

**4. Quitar la recreación de memoria por frame del hilo FIFO**
- Cambios: reservar la primera pasada, conservar capacidad por slot y dar al FrameOp un rango en lugar de copiar
  punteros.
- Beneficio: 0,2–0,8 ms [I].
- Riesgo: bajo-medio (vida del slot).

**5. Granularidad de publish/despertar**
- Cambios: despertar solo si el worker duerme y agrupar publishes pequeños, publicando siempre en los puntos de
  sincronización.
- Beneficio: 0–1 ms.
- Riesgo: bajo, pero puede retrasar el arranque del consumidor.

**6. Coste por draw del render worker**
- Cambios: pipeline cacheado por pasada sin mutex global; índice enlazado una vez por pasada.
- Condición: solo si la nueva medición de espera de slot en Begin lo justifica.
- Beneficio: 0,5–2 ms de render worker, que el frame solo gana si está limitado por el render.

## Propuesta para H35 (un solo NRO)

- **Optimización principal:** la **1** (reutilización de uniforms en cambios de pipeline). Es código de la propia ruta
  caliente del hilo FIFO, tiene una prueba de equivalencia exacta en host y entra detrás de un interruptor de
  diagnóstico, como el memo de H31, para medir A y B con el mismo NRO.
- **Secundaria estrechamente relacionada:** la **2** (afinidad del audio y los OSThread). Actúa sobre el mismo recurso,
  el tiempo que el núcleo 1 dedica a otra cosa que traducir el frame. Va detrás de otro interruptor (apagado por
  defecto) y solo se activa en la prueba si la instrumentación muestra ≥ 0,5 ms/frame de audio u OSThreads en el
  núcleo 1.
- **Instrumentación (imprescindible y en el mismo NRO)**, en el informe del benchmark (subir a esquema 4 con campos
  añadidos):
  1. `fifo_busy_ms`: tiempo alrededor de `process()` en `process_to` (`lib/gx/fifo.cpp:55-63`), acumulado en un
     atómico relajado.
  2. `drain_wait_ms` por sitio (`GXDrawDone`, `end_frame`, doble pantalla/mods), con el tiempo del bucle de
     `fifo.cpp:256-262`. De ahí `drawrec_net = DrawRec − drain_wait[GXDrawDone]` y `painter_net = Painter − TickWait`.
  3. Publishes y despertares reales por frame; bytes de FIFO por frame.
  4. Contadores de `refresh_uniform`: rellenos, aciertos de repetición y reutilizaciones.
  5. Tiempo ocupado del render worker y espera de slot/staging (de TracyPlot a `AuroraStats`).
  6. ms del callback de audio por segundo.
- **Protocolo A/B:** F_SP122 sala 8, caché caliente, misma interpolación anotada, 3 ejecuciones por brazo
  (principal OFF/ON, y la secundaria solo si procede). Comparar `work_ms`, `drawrec_net`, `drain_wait`, `fifo_busy`,
  `painter_net` y `render_busy`. Solo cuenta lo que supere el ruido de ejecución a ejecución (2,4 %, H28).
- **Aceptación de H35:** `fifo_busy` −0,5 ms o más con la principal, sin diferencias visuales ni discrepancias en la
  verificación, y FPS/p95 iguales o mejores que H33/H34.
- **Reversión:** cualquier diferencia visual, discrepancia en el modo de verificación o, para la secundaria, cualquier
  artefacto de audio.

Antes de H35 hay que comprobar si la captura del HUD de doble pantalla estaba activa en las ejecuciones de referencia.
Si lo estaba, añade drenajes dentro de Painter.

---

## Implementación H35 (H35-c1)

**Sin compilar ni probar en consola.** Todo lo de esta sección está hecho y probado en host (pruebas unitarias,
ASan/UBSan, valgrind, comprobación de sintaxis de las unidades del juego con y sin `-D__SWITCH__`). Nada de lo que
sigue es una medición de Switch; las cifras de hardware son las de H34-c2.

### Ramas y commits

| Repositorio | Rama | Base | Commits H35 |
|---|---|---|---|
| principal | `claude/h35-fifo-tour` | `7f2c328c9f` (H34-c2) | Ordon; telemetría en el informe; `painter_net` solo si Painter corrió; Performance Tour; docs; gitlink |
| `aurora-aleks-nx` | `claude/h35-fifo-uniforms` | `0020cbf636` | stubs de `gx_fifo_tests`; telemetría FIFO; reutilización de uniforms; esperas de frame |

El gitlink de `extern/aurora` pasa de `0020cbf6365c171e1f25988567e1d3a51168b7bc` a
`845a511c5b33588118554be2b410734f8c2cf2eb` en un commit propio, el último. Los commits intermedios de la rama
principal usan la API de Aurora H35: compilan solo con ese gitlink. Borealis, Mesa, Dawn, el backend, las
afinidades y los demás submódulos no se tocaron.

### Ordon (fase 1)

`shader_prep.cpp` entraba en Ordon Village por `F_SP103` sala 0 **punto 0**. Según la tabla de entradas del
Randomizer (`generator/data/entrance_shuffle_data.yaml` en `TwilitRealm/dusklight-randomizer@1f24e81`, el commit que
fija el submódulo; etapa 43 = `F_SP103` en `src/stages.cpp`), el punto 0 de la sala 0 es «Ordon Ranch Village
Pathway -> Ordon Village» y el punto 1 «Outside Links House -> Ordon Village». En una partida nueva el punto 0 dispara
la secuencia de las cabras y sale a `F_SP00`: es lo que pasó en H34-c2. Prepare Shaders y el Performance Tour usan el
**punto 1**. La prueba de Prepare Shaders exige ahora etapa, sala, punto y capa exactos (antes solo que el punto
existiera en la tabla, y los dos existen); con el punto 0 falla.

### Telemetría FIFO (Aurora)

`aurora_gx_fifo_stats()` en `include/aurora/tuning.h`, mismo patrón que `AuroraPipelineMemoStats`: contadores
acumulados desde el arranque, nunca se ponen a cero; quien lee resta dos lecturas (la resta sin signo sigue siendo
correcta si un contador da la vuelta).

| Contador | Dónde | Hilo que escribe |
|---|---|---|
| `busy_ns`, `processed_bytes`, `process_calls` | alrededor de `process()` en `process_to` (`lib/gx/fifo.cpp`); excluye el callback de draw-done | worker FIFO |
| `worker_sleeps` | cada vez que el worker no tiene trabajo y espera | worker FIFO |
| `publishes` | cada avance de la marca publicada (`publish()` y `drain()`) | juego |
| `wake_requests` | cada `notify_all` al worker | juego |
| `drains` y `drain_sites[6]` | `calls` (todas, también vacías), `waits` (las que bloquearon) y `wait_ns` (solo el bucle de espera) | juego |

Los dos grupos (worker y juego) están en líneas de caché separadas (`alignas(64)`). Son atómicos relajados; el reloj
(`steady_clock`) se lee alrededor de trabajo que ya existía y del bucle de espera que ya existía, y en `drain()` solo
si de verdad espera. No se añadió ningún mutex ni se cambió el orden, la publicación ni la sincronización.

Clasificación de cada `fifo::drain()` (revisados todos los sitios):

| Sitio | Categoría | Comentario |
|---|---|---|
| `GXDrawDone` (`lib/dolphin/gx/GXManage.cpp`) | `gx_draw_done` | el de cada frame está en `JFWDisplay::endFrame`, dentro de DrawRec; también lo llaman `m_Do_Reset` y `d_a_movie_player` |
| `aurora::end_frame` (`lib/aurora.cpp`) | `end_frame` | se espera ≈ 0, porque DrawRec acaba de drenar [I]; el informe lo dirá |
| `AuroraGXSync` (mods) y `gfx::drain_fifo()` | `explicit` | |
| `create_pass`, `resolve_pass`, `push_custom_draw`, `push_encoder_task` (`lib/gfx/recording.cpp`) | `aux_pass` | |
| captura del HUD del companion (`src/dusk/dualscreen.cpp`) | `companion_capture` | la captura etiqueta con `aurora_gx_fifo_set_drain_tag` (RAII) sus tres drenajes: `create_pass`, `drain_fifo` y `resolve_pass`; la etiqueta solo cambia la contabilidad |
| cualquier otro | `other` | |

Además, aislado y pequeño: `aurora_gfx_frame_wait_stats()` con la espera de slot de frame y de staging en
`aurora_begin_frame` (la misma ventana que antes solo iba a Tracy) y el tiempo del render worker ejecutando su cola
(incluye `present`, que puede bloquear en la pantalla: no es CPU pura). El tiempo del callback de audio **no** se mide
en H35: está en `src/dusk/audio/DuskAudioSystem.cpp` y se añadiría igual (dos lecturas de reloj alrededor del callback
y un atómico relajado); no se hizo para no tocar el audio en este hito.

### Informe del benchmark, esquema 4

Objetos nuevos (todos de la ventana de medida, fin menos inicio; «por frame» divide entre los frames del profiler):

- `fifo`: `processing_mode`, `frames`, `worker_busy_ms_total`, `worker_busy_ms_per_frame`, `processed_bytes_total`,
  `processed_bytes_per_frame`, `process_calls_total/_per_frame`, `publishes_total/_per_frame`,
  `worker_wakes_total/_per_frame`, `worker_sleeps_total/_per_frame`, `drain_calls_total/_per_frame`,
  `drain_waits_total`, `drain_wait_ms_total`, `drain_wait_ms_per_frame` y `drains_by_site` con, para
  `gx_draw_done`, `end_frame`, `companion_capture`, `explicit`, `aux_pass` y `other`: `drain_calls`,
  `drain_calls_per_frame`, `drain_waits`, `drain_wait_ms`, `drain_wait_ms_per_frame`.
- `uniforms`: `mode`, `mode_at_start`, `mode_changed_during_window`, `full_builds(_per_frame)`,
  `repeat_hits(_per_frame)`, `reuse_candidates(_per_frame)`, `reuses(_per_frame)`, `verification_checks`,
  `verification_mismatches`, `disabled_after_mismatch` (y, si está a `true`, `last_mismatch_section` y
  `last_mismatch_offset`).
- `frame_waits`: `frame_slot_waits`, `frame_slot_wait_ms_per_frame`, `staging_waits`, `staging_wait_ms_per_frame`,
  `render_items`, `render_work_ms_per_frame`.
- `derived`: `execute_ms_per_frame`, `painter_ms_per_frame`, `actors_ms_per_frame`, `tick_wait_ms_per_frame`,
  `draw_ms_per_frame`, `painter_net_ms_per_frame` (= Painter − TickWait), `painter_net_clamped`,
  `painter_net_available`, `drawrec_ms_per_frame`, `gx_draw_done_wait_ms_per_frame`, `drawrec_net_ms_per_frame`
  (= DrawRec − espera de drain de `GXDrawDone`), `drawrec_net_clamped`, `drawrec_net_available`. Los netos nunca son
  negativos: las zonas y la espera vienen de relojes distintos sobre tramos casi iguales, y una diferencia negativa se
  escribe 0 con `*_clamped: true`. Con interpolación, Painter no corre: `painter_net_available: false`.
- `interpolation` gana `frames_total`, `frames_normal`, `frames_normal_while_stalled`,
  `frames_interpolated_accepted`, `frames_interpolated_with_sim_tick`, `frames_interpolated_rejected`, `sim_ticks`,
  `sim_rate_hz`, `presented_rate_fps`. Se cuentan por frame en `game_clock::advance()` (`interp_frames.hpp`), no se
  infieren del FPS: *aceptado* = interpolando sin tick de simulación pendiente; *con tick* = interpolando y corrió al
  menos un tick; *rechazado* = la predicción durmió hasta el tick en vez de dibujar un frame interpolado; *normal* =
  sin interpolación (apagada o parada por el gobernador).
- `settings.uniform_reuse_mode`.

El resto (perfil, resolución efectiva, portátil/dock, Flip Grip, companion, interpolación, caché/pipelines, clocks y
mods) sigue igual. Los netos `drawrec_net` incluyen todos los `GXDrawDone` de la ventana, casi todos dentro de DrawRec.

### Reutilización del bloque de uniforms

#### Auditoría completa de `fill_uniform` (`lib/gx/shader_info.cpp`)

El bloque se escribe por secciones, en este orden. «Escrita si» es lo que decide la **estructura**; «Estado leído» lo
que decide el **contenido**. Todos los escritores de esos campos están en `lib/gx/regs.cpp`, `lib/gx/gx.cpp` y
`lib/gx/command_processor.cpp` (búsqueda de asignaciones a cada campo en todo `lib/`).

| Sección | Bytes | Escrita si | Estado leído | Quién lo escribe | Cubierto por |
|---|---|---|---|---|---|
| viewport | 16 | siempre | `renderViewport.width/height`, `logicalViewport.width/height` | `set_render_viewport` (`GX_AURORA_LOAD_VIEWPORT_RENDER`, `set_logical_viewport`), `set_logical_viewport` (XF 0x101A-0x101F) | `DirtyUniform` si cambian `left`, `width` o `height` (`gx.cpp`) |
| line_point | 16 | `lineMode != 0` (puntos si `== 3`) | `pointSize`, `pointTexOffset`, `lineWidth`, `lineHalfAspect`, `lineTexOffset`, `texCoordScales[].pointOffset/lineOffset` | BP 0x22; BP 0x30-0x3F | `DirtyUniform` (tabla `kBpRegs`) |
| projection | 64 | siempre | `proj` (`UseReversedZ` es constante) | XF 0x1020-0x1026; `GX_AURORA_LOAD_PROJECTION_FULL` | `DirtyUniform` |
| pos_mtx | 480 | siempre | `pnMtx[0..9].pos` | `copy_xf_data` 0x000-0x077 (XF directo y `LOAD_INDX_A`) | `DirtyUniform` si cambia un bit |
| tex_mtx | 480 | siempre | `texMtxs[0..9]` | `copy_xf_data` 0x078-0x0EF | `DirtyUniform` |
| nrm_mtx | 480 | siempre | `pnMtx[0..9].nrm` | `copy_xf_data` 0x400-0x459 | `DirtyUniform` |
| tev_regs | 16 × n | bits de `loadsTevReg` | `colorRegs[i]` | BP 0xE0-0xE7 (bit 23 = 0) | `DirtyUniform` |
| lights | 656 | `lightingEnabled` | `lights[0..7]`, `colorChannelState[0..3].lightMask` | `copy_xf_data` 0x600-0x67F; XF 0x0E-0x11 | `DirtyUniform`; `DirtyPipeline \| DirtyUniform` |
| channel_colors | 16 × n | bits de `sampledColorChannels` **y**, por canal de color y de alfa, `colorChannelConfig.lightingEnabled && ambSrc == REG` y `matSrc == REG` | `colorChannelState[].ambColor/matColor` | XF 0x0A-0x0D (colores); XF 0x0E-0x11 (configuración) | colores: `DirtyUniform`; **configuración: no está en `ShaderInfo`; va en la firma** |
| kcolors | 16 × n | bits de `sampledKColors` | `kcolors[i]` | BP 0xE0-0xE7 (bit 23 = 1) | `DirtyUniform` |
| pt_tex_mtx | 960 | `usesPTTexMtx.any()` (escribe las 20) | `ptTexMtxs[0..19]` | `copy_xf_data` 0x500-0x5EF | `DirtyUniform` |
| fog | 80 | `usesFog` | `fog.color/a/b/c/rangeCenter/rangeK`, `logicalViewport.left/width`, `renderViewport.left/width` | BP 0xE8-0xF2; viewports | `DirtyUniform` (0xF1 también `DirtyPipeline`) |
| texcoord_scales | 128 | siempre | `texCoordScales[0..7].scaleS/scaleT` | BP 0x30-0x3F | `DirtyUniform` |
| ind_tex_mtx | 96 | `usedIndTexMtxs.any()` (escribe las 3) | `indTexMtxs[0..2].mtx`, `scaleExp` | BP 0x06-0x0E | `DirtyUniform` |
| texture_sizes | 16 × n | bits de `sampledTextures` | `textures[i].texObj` (ancho, alto, `lod_bias` de `mode0`), `ref->hasArbitraryMips`, `ref->size.width`, viewports | `resolve_sampled_textures` (desde `refresh_bind_groups`); un reemplazo de textura cambia `ref` | **ninguna bandera lo garantiza**: con el mismo bind group no se marca `DirtyUniform`. **Se recalcula y se compara** |

`DirtyUniform` solo se borra en `refresh_uniform`, y un draw solo se fusiona con el anterior si `dirty == 0`.

Campos de `ShaderInfo` que deciden estructura o contenido: `lineMode`, `loadsTevReg`, `lightingEnabled`,
`sampledColorChannels`, `sampledKColors`, `usesPTTexMtx.any()`, `usesFog`, `usedIndTexMtxs.any()`, `sampledTextures`
(y `uniformSize`, que solo reserva memoria pero se incluye). No influyen: `sampledTexCoords`, `writesTevReg`,
`indexAttr`, `usedIndStages`, `sampledIndTextures` (solo bind groups) ni qué bits concretos de `usesPTTexMtx` y
`usedIndTexMtxs` (se escriben siempre todas las matrices).

#### Firma y condiciones

Un draw cuyo uniform está sucio reutiliza el bloque anterior **solo si se cumplen todas**:

1. **Solo cambió el pipeline.** `push_gx_draw` mira `DirtyUniform` antes de `refresh_pipeline`: si ya estaba
   (escritura de estado GX), no es candidato. Si lo puso `refresh_pipeline`, y `refresh_bind_groups` no lo puso
   (devuelve si cambió el bind group), sí.
2. **Misma clave de estructura** que el último bloque llenado entero: todos los campos de `ShaderInfo` de arriba más
   los dos predicados de la configuración de canal **viva** (`colorChannelConfig`, por canal muestreado, color y alfa),
   leídos del estado en el momento, como hace `fill_uniform`.
3. **Misma sección de texturas**, recalculada exactamente como la escribe `fill_uniform` (memcmp de los `Vec4`).
4. **Mismo búfer:** misma época (`forget_last_uniform` la avanza al empezar un frame) y la caché de draw guarda
   todavía el rango de ese bloque (`clear_draw_cache` al final del frame lo borra; el seguimiento también).

Argumento de equivalencia: los bytes de `fill_uniform` son función de (clave de estructura, valores de la tabla).
Desde el último llenado completo, (1) garantiza que ningún valor cubierto por `DirtyUniform` cambió: cualquier cambio
habría marcado `DirtyUniform`, que solo se borra en `refresh_uniform`, y ese `refresh_uniform` habría sido un llenado
completo (una reutilización exige que no estuviera marcado). (2) cubre la estructura y la configuración de canal, que
no tiene bandera propia para el uniform. (3) cubre las texturas, que no la tienen. (4) garantiza que el rango sigue
siendo del búfer actual. Lo único que esto supone es lo que **ya** supone la ruta de H34 cuando el pipeline no cambia:
que todo cambio de estado de la tabla marca `DirtyUniform`. La auditoría de arriba lo comprueba escritor por escritor, y
Verify existe para detectar en consola cualquier hueco.

Comportamientos previos que no cambian y valen igual en los tres modos: `UniformRepeat` sigue comparando tamaño y hash
xxh3 de 64 bits (no byte a byte) antes de empujar un bloque repetido; y `resolve_sampled_textures` no refresca un
`TextureBind` con el mismo `texObjId` y versión aunque cambie `mode0`.

#### Modos (`aurora_gx_set_uniform_reuse_mode`)

- **Off** (por defecto en H35): exactamente `build_uniform` de H34. Prueba diferencial
  (`tests/h35/run-uniform-off-equivalence.sh` en Aurora): el mismo controlador aleatorio, 200 000 draws, enlazado con el
  `shader_info.cpp` de `0020cbf` y con el de H35 en Off, da el mismo resumen de todos los bloques empujados y de todos
  los rangos (`8614efd24a891ad4`). `fill_uniform` solo ganó marcas de sección que en el llenado normal no hacen nada.
- **Verify:** decide como On, pero siempre llena el bloque entero, lo compara byte a byte con el que se habría
  reutilizado y **dibuja el completo**. Una diferencia cuenta `verification_mismatches`, registra en el log (las 8
  primeras) la sección y el byte de la primera diferencia, los tamaños y la clave, y desactiva la reutilización para el
  resto del proceso (`disabled_after_mismatch`): esa ejecución ya no puede pasar a On en silencio.
- **On:** aplica la reutilización; nunca después de una discrepancia.

El modo se elige en **Ajustes › Diagnostics (Aleks) › Uniform Reuse (H35)**, solo en ejecución: no se escribe en
`config.json` y cada arranque empieza en Off. Cambiarlo durante el juego es seguro: cada draw lee el modo una vez, una
reutilización solo apunta a un bloque llenado antes en el mismo frame y en las mismas condiciones, y Off invalida el
seguimiento en cada llenado. El benchmark y el tour registran el modo (y si cambió en la ventana).

#### Pruebas (Aurora, `gx_uniform_tests`, con el `shader_info.cpp` real)

Un oráculo independiente que escribe el bloque desde el estado GX; pipeline distinto con uniforms idénticos y
distintos; tamaño de textura y LOD bias; matrices de posición, luces, niebla, KColors, registros TEV, matrices
indirectas y de post-transformación, escalas de coordenadas; configuración de canal viva; primer draw; cambio de layout
del render target; principio y final de frame; Verify sin discrepancias; una discrepancia artificial (un K color
cambiado sin bandera) que cae al bloque correcto y desactiva la reutilización; cambios de modo a mitad de frame;
secuencias aleatorias en los tres modos; y casos de extremo a extremo por el worker FIFO real. Mutaciones: quitar
cualquiera de las condiciones (configuración de canal, sección de texturas, «solo pipeline», época, KColors de la
clave) hace fallar al menos una prueba. 20/20 en normal y con ASan/UBSan, limpio en valgrind, 2000 ejecuciones bajo
carga sin fallos. Una sola ejecución, la primera del binario recién enlazado, falló una vez en el caso de extremo a
extremo en **Off** contra el oráculo y no se ha podido reproducir (≈ 3900 ejecuciones después, valgrind limpio); como
Off es idéntico a H34 por la prueba diferencial, no puede venir de la reutilización, pero no está explicado.

#### Criterio para conservarla (en H36)

Equivalencia byte a byte (Verify con 0 discrepancias en la consola), Off igual a H34 (hecho), pruebas host completas
(hecho) y una ganancia medida en `fifo.worker_busy_ms_per_frame` que supere el ruido (2,4 %, H28), sin diferencias
visuales. H35 no cambia el valor por defecto; H36 podrá promover On si se valida.

### Run Performance Tour

**Ajustes › Diagnostics (Aleks) › Run Performance Tour.** Un cuarto tipo de ejecución en `benchmark.cpp`; la parte sin
juego (plan, reglas, seguimiento de la recuperación, informe) está en `src/dusk/perf_tour.cpp`.

| # | Zona | Etapa / sala / punto / capa | Clase | Evidencia |
|---|---|---|---|---|
| 1 | Outside Castle Town - West | `F_SP122` / 8 / 0 / −1 | pesada | tabla de warp; escena del benchmark desde H28; Randomizer: «Lanayru Field -> Outside Castle Town West», etapa 57 sala 8 punto 0 |
| 2 | Ordon Village | `F_SP103` / 0 / 1 / −1 | media | tabla de warp; Randomizer: «Outside Links House -> Ordon Village», etapa 43 sala 0 punto 1 |
| 3 | Link's House | `R_SP01` / 4 / 0 / −1 | ligera | tabla de warp; Randomizer: «Outside Links House -> Ordon Links House», etapa 65 sala 4 punto 0 |

El orden pesada → media → ligera es deliberado y **el gobernador no se reinicia entre zonas**: si se para en
`F_SP122`, el informe muestra si se recupera, y cuánto tarda desde la llegada, en las zonas ligeras.

Por zona: warp; carga (límite 120 s; si no carga, la zona queda `failed` y se sigue); cola de pipelines a cero durante
1 s (límite 45 s, que se registra); 9 s de calentamiento; 30 s de medida; resultado guardado; siguiente zona.
Estimación, no medida: 3–5 minutos (≈ 3 × (15 s de carga + 2 s de cola + 39 s) + vuelta). Usa el perfil, la resolución, el companion, el Flip
Grip, la interpolación y el modo de uniforms actuales; no fuerza un perfil y no escribe ajustes.

Protecciones compartidas (`diagnostic_run.hpp`): bloqueo de guardado; suspensión de logros; cierre de Ajustes **antes**
de leer las líneas base; copia del estado de guardado en memoria antes del primer warp; vuelta al lugar original y
recolocación de Link; Start o un menú cancelan; `begin_restore()` en toda salida; rehabilitación de guardado; informe
con el jugador ya de vuelta; logros reanudados lo último. Nuevo en H35: el bloqueo de guardado es un guardián que
posee el `Run` (`diagnostic_run::ScopedFlag`), así que un `Run` destruido sin pasar por el final ya no puede dejar el
guardado desactivado; está declarado después de la suspensión de logros para que, destruido, devuelva el guardado antes
de reanudar los logros.

Informe: `<datos>/benchmarks/tour-<UTC>-<build>.json`, esquema `dusklight-aleks-performance-tour` v1:
`build` (id, tag, commit, `tree_dirty`, fecha), `run` (estado, motivo, inicio, duración, orden, sin reinicio del
gobernador), `original_state`, `configuration` (entorno con modo, Flip Grip, resolución, perfil, companion y clocks;
interpolación; modo de uniforms; memo; FPS máximo; vsync; API, adaptador, driver; pipelines al empezar), `mods`,
`zones_summary` y, por zona: `performance` (frames, FPS medio, 1 % low, frametime medio/mediano/p95/p99/máximo,
% > 33,3 ms y > 50 ms, judder), `cpu_zones_ms`, `derived`, `fifo`, `uniforms`, `frame_waits`, `interpolation` (modo
pedido, calidad, predictiva, tipos de frame, tasa de simulación y presentada, entradas y salidas de `stalled`,
intentos y fallos de recuperación, segundos y % de la ventana `stalled`, estado al llegar, al empezar y al acabar la
medida, `recovery_seconds_from_arrival`), `loading` (carga, pipelines al llegar, máximo, creados, tiempo de cola,
timeout, caché `warm`/`cold`/`compiling`) y `environment`; después `summary` (`consistent_environment`,
`heavy_zone_stalled`, `light_zone_arrived_stalled`, `light_zone_recovered`, `light_zone_recovery_seconds` y una línea
por zona) y `restore` (`returned_to_original_place`, `in_memory_save_state_restored`, `saves_reenabled`,
`config_saves_during_run`, `config_json_written`, `achievements_suspended_during_run`).
`consistent_environment: false` dice que entre zonas cambió el modo, el Flip Grip, el companion, el perfil, la
resolución o el modo de uniforms: esas zonas no se comparan.

### Pruebas ejecutadas (host)

| Pruebas | Resultado |
|---|---|
| Aurora `gx_fifo_tests` (incluye `gx_fifo_stats_test.cpp`) | 216/217, normal y ASan/UBSan; la que falla ya fallaba en `0020cbf` (`CommandsAfterFinalDrawRemainPendingUntilDrain` espera un publish por draw, y este fork publica cada 16) |
| Aurora `gx_uniform_tests` | 20/20, normal, ASan/UBSan y valgrind; 2000 repeticiones bajo carga |
| Aurora `render_worker_tests` | 6/6 |
| Aurora `tests/h35/run-uniform-off-equivalence.sh` | mismos resúmenes que `0020cbf` |
| Principal, los 14 `platforms/switch/tools/test-*.sh` | todos pasan; nuevos o ampliados: `test-shader-prep.sh`, `test-benchmark-report.sh`, `test-diagnostic-run.sh`, `test-perf-tour.sh` (los tres últimos con ASan/UBSan y los JSON pasados por `json` de Python) |
| Sintaxis (`-fsyntax-only`, host y `-D__SWITCH__` con un `switch.h` mínimo) | `benchmark.cpp`, `game_clock.cpp`, `dualscreen.cpp`, `ui/settings.cpp`, `perf_tour.cpp`, `fifo_report.cpp`; en Aurora `frame.cpp`, `recording.cpp`, `aurora.cpp`. **No es una compilación de Switch** |

Para compilar las pruebas de Aurora en la nube hizo falta un arnés propio (CMake con gtest, abseil y xxhash del
sistema, fmt 12.1.0, Tracy 0.14.1 y las cabeceras de Dawn generadas del commit que fija Aurora): el
`tests/CMakeLists.txt` de Aurora necesita el árbol completo de dependencias. El arnés no está en ningún repositorio; los
objetivos nuevos sí están en `tests/CMakeLists.txt`.

### Compilar H35-c1 en local (una sola vez)

```
git fetch origin claude/h35-fifo-tour
git checkout claude/h35-fifo-tour
git submodule update --init extern/aurora
git -C extern/aurora rev-parse HEAD   # debe ser 845a511c5b33588118554be2b410734f8c2cf2eb
git status --short                    # vacío: BuildIdentity registra dirty=0
cmake --preset switch-devkitA64-release \
  -DALEKS2_STATIC_MODS="template_mod;flow_demo;rollandsprint" \
  -DNAGA_VULKAN_ROOT=<ruta>/mesa-naga-2623/opt/devkitpro/portlibs/switch \
  -DALEKS2_BUILD_TAG=H35-c1
cmake --build --preset switch-devkitA64-release
```

Configurar **después** del checkout (el commit se lee al configurar) y en un directorio de build limpio o con la caché
de `ALEKS2_BUILD_TAG` actualizada. Antes de compilar, opcional y recomendable: `bash platforms/switch/tools/test-*.sh`
(los cuatro nuevos o ampliados necesitan `extern/aurora` inicializado). Anotar el SHA-256 del NRO.

### Protocolo de prueba en hardware (H35-c1, ≈ 60–75 min)

Antes: copia de `config.json`, `achievements.json` y las partidas; hash de `config.json` y `achievements.json`;
portátil, sin overclock; **companion apagado** salvo en el paso 7 (las cifras de H34-c2 se tomaron así); anotar si el
Flip Grip está activo. Uniform Reuse en **Off** al arrancar (debe venir así).

1. **Arranque (5 min):** sin errores en el log; en Diagnostics aparecen *Uniform Reuse (H35)* = Off y *Run
   Performance Tour*.
2. **Prepare Shaders (≤ 5 min, caché caliente):** Ordon Village debe quedar `completed` sin la secuencia de las cabras
   y sin `F_SP00`; Link's House carga. `config_json_written: false`.
3. **Benchmark, Off (2 min):** informe con `schema_version: 4` y los objetos `fifo`, `uniforms`, `frame_waits` y
   `derived`. Comparar con H34-c2 (Quality 1280×720: 29,43 FPS, DrawRec ≈ 10,7–11,3 ms): debe quedar dentro del ruido.
   Anotar `fifo.worker_busy_ms_per_frame`, `drains_by_site.gx_draw_done.drain_wait_ms_per_frame` y
   `derived.drawrec_net_ms_per_frame`.
4. **Performance Tour, Off, dos veces (≈ 10 min):** repetibilidad por zona; `restore` correcto; misma posición.
5. **Verify, una vez (≈ 5 min):** Uniform Reuse = Verify y tour. Debe dar `verification_mismatches: 0`,
   `disabled_after_mismatch: false` y ningún `uniform reuse: Verify mismatch` en el log. Si hay una sola discrepancia:
   **no seguir a On**; guardar log y JSON (sección y byte) para H36.
6. **On (≈ 15 min):** tour dos veces y benchmark una vez; 2–3 minutos de juego normal mirando Hyrule Field, Ordon y la
   casa de Link por si hay diferencias visuales (luces, niebla, texturas, HUD).
7. **Companion encendido (opcional, ≈ 5 min):** un tour en Off con el companion activo, para medir
   `companion_capture` aparte. No mezclar con los anteriores.
8. **Después de cada ejecución:** la misma posición, guardar funciona, hash de `config.json` y de `achievements.json`
   igual al de antes.

Qué mirar para decidir H36: `fifo.worker_busy_ms_per_frame` y `derived.drawrec_net_ms_per_frame` Off frente a On
(misma zona y entorno, `consistent_environment: true`); `uniforms.reuses_per_frame` frente a `full_builds_per_frame`;
`interpolation` de cada zona (si `F_SP122` entra en `stalled` y si Ordon o la casa de Link se recuperan, y en cuánto);
`frame_waits` (si el límite es el render worker y no el FIFO). Aceptar On en H36 solo con 0 discrepancias, sin
diferencias visuales y con una mejora por encima del 2,4 %.

### Riesgos y trabajo pendiente

- Nada de esto se ha compilado para Switch; la comprobación de sintaxis usa un `switch.h` mínimo.
- El coste de la telemetría (dos lecturas de reloj por `process()` y por espera de `drain`, ≈ 100–200 por frame) no
  está medido en consola; se espera muy por debajo del ruido.
- La reutilización depende de la auditoría de banderas: un escritor de estado futuro que no marque `DirtyUniform`
  rompería también la ruta de H34 cuando el pipeline no cambia; Verify lo detectaría.
- El fallo único no reproducido de la prueba de extremo a extremo en Off (arriba).
- No se midió el callback de audio ni se tocaron afinidades (la oportunidad 2 sigue pendiente, fuera de H35).
- La duración real del tour depende de las cargas en consola.
