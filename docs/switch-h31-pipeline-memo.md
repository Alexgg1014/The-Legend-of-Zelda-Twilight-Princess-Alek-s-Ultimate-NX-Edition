# H31: memo de búsqueda de pipeline

Una sola optimización sobre la base H30 (Mesa 26.2.3). Candidato `H31-c1`; no validado en hardware.

## Qué hace

`push_gx_draw` (`extern/aurora/lib/gx/command_processor.cpp`) resuelve un `PipelineRef` y un `ShaderInfo` cada vez que la configuración de pipeline cambia de verdad (986 veces por frame en F_SP122). En H30 cada resolución hace `load_shader_info` (hash de `ShaderConfig`, 2720 B) y `gfx::pipeline_ref`, que a su vez hace dos hashes de `PipelineConfig` (2772 B), dos cerrojos, una copia de 2772 B dentro de un `KnownPipeline` y otra dentro de una lambda con `std::function`, incluso cuando el pipeline ya existe. H31 guarda el resultado de la primera resolución en un memo del hilo FIFO y en las repeticiones devuelve el valor guardado tras un hash y un `memcmp`.

## Por qué la primera llamada normal preserva prioridad y persistencia

Se verificó contra `gfx/pipeline_cache.cpp`:

- La primera aparición de una combinación recorre siempre `find_pipeline` completo: `remember_pipeline_config` (fija `firstFrameUsed = min(...)` y encola la escritura a SQLite si es nueva o más temprana) y `resolve_pipeline` → `find_pipeline_impl`.
- En `find_pipeline_impl` (prioridad Normal) el pipeline queda en uno de tres estados: ya existe en `g_pipelines`; se crea; o queda pendiente. Si estaba pendiente en la cola de fondo, `promote_pending_pipeline` lo pasa **una sola vez** a la cola normal; llamadas posteriores no lo encuentran en la cola de fondo y no hacen nada. Si no existía, se encola en la cola normal. En los tres casos la llamada posterior sería un acierto sin efecto: `remember_pipeline_config` no cambia `firstFrameUsed` (el fotograma solo crece) ni encola escrituras, y `find_pipeline_impl` solo repetiría un `notify_one` a un worker que ya tiene la cola.
- Un pipeline pendiente siempre acaba en `g_pipelines` (`pipeline_worker` lo mueve y lo borra de `g_pendingPipelines`); nada lo descarta salvo `shutdown_pipeline_cache`. Por eso no hace falta esperar a que esté compilado para memoizar. Lo único que borra o re-encola cosas es inicializar, apagar o reconstruir la caché, y ahí se invalida el memo.

## Dónde se invalida

`gfx::pipeline_generation()` (un contador atómico en `pipeline_cache.cpp`) se incrementa al inicio de `initialize_pipeline_cache`, `shutdown_pipeline_cache` y `rebuild_pipeline_cache` (que cubren cambio de dispositivo y de layout de render target). El memo guarda la generación con la que se llenó; si no coincide en una consulta, se vacía (sin asignar memoria) y esa consulta es un fallo. No se vacía por fotograma. `targetLayoutKey` forma parte de la clave.

## Capacidad y memoria

1024 entradas, 256 conjuntos × 4 vías. Una entrada guarda `PipelineConfig` (2772 B), `ShaderInfo` (96 B), `PipelineRef`, hash, `targetLayoutKey`, tipo, versión y sello: ~2,95 KB. Máximo ~3,0 MB, reservado una sola vez en el primer fallo (nunca en un acierto). Reemplazo: LRU determinista dentro del conjunto (primero una vía libre, si no la de sello más bajo). Los 649 pipelines conocidos incluyen los de otras escenas y del layout fuera de pantalla, por lo que 1024 deja holgura.

## Colisión de hash

Un acierto exige, además del hash: mismo tipo (GX), mismo `targetLayoutKey`, misma versión de config y **igualdad byte a byte** de los 2772 B del `PipelineConfig` (`memcmp`; la estructura tiene representaciones únicas, lo comprueba un `static_assert`). Una colisión con otra configuración es un fallo y recorre el camino normal.

## Hilo, cerrojos, asignación

Solo lo toca el hilo FIFO (`push_gx_draw`). Un acierto no toma cerrojos ni asigna. Los contadores son atómicos de un solo escritor (lectura y escritura relajadas, sin RMW) para poder leerlos desde el hilo principal.

## Camino desactivado

Con el diagnóstico apagado `push_gx_draw` ejecuta la búsqueda completa (`build_shader_info` y `gfx::pipeline_ref`), sin consultar ni modificar el memo y sin contar. (En H31 esa búsqueda llevaba delante una caché de `ShaderInfo` por hash; H32 la retiró con el resto del código de la base anterior, así que desde H32 el camino apagado ya no es el de H30. Con el memo encendido, que es el predeterminado, no cambia nada: solo llama a la búsqueda en el primer fallo de cada configuración.) El coste del toggle en el camino caliente es una lectura atómica relajada por cambio de configuración.

## Toggle y reporte

Diagnóstico `Pipeline Memo (H31)` en `Ajustes > Aleks > Diagnostics`, `game.pipelineMemo`, predeterminado encendido; no es un ajuste gráfico de usuario. El JSON añade `settings.pipeline_memo` y, en `gfx`, `pipeline_memo_hits`, `pipeline_memo_misses`, `pipeline_memo_evictions`, `pipeline_memo_hit_rate`, `pipeline_memo_max_occupied` y `pipeline_memo_capacity` (hits, misses y evictions son de la ventana; el máximo de entradas es el acumulado).

## Pruebas host

`platforms/switch/tools/test-pipeline-memo.sh` (acierto exacto, colisión, layout, versión, tipo, callback una vez, generación, apagado, capacidad/evicción, estadísticas) y `test-benchmark-report.sh`.

## Resultado y validación en hardware (2026-10-01)

**H31-c1** (`H31-c1-e8a3c8a4b8`, SHA-256 `2179237f23d0169bfa6c3cb777abad3f86f77b12ca05aa053a38e3dcd49642fb`): validado en hardware por el usuario; sin problemas visuales; log sin errores, advertencias ni device loss; benchmark, restauración y guardados correctos. Cumple el criterio para conservar el memo y queda como base de rendimiento actual; H30-c1 queda como fallback (con el memo apagado el camino era exactamente el de H30 en H31; desde H32 ya no, ver arriba).

Una ejecución caliente en F_SP122 sala 8, Flip Grip 733x540, clocks stock de portátil, Mesa 26.2.3, memo encendido, contra H30:

| | H30 | H31 |
|---|---|---|
| FPS medio | 26,69 | 29,27 |
| 1 % low | 17,54 | 17,42 |
| p95 / p99 | 53,6 / 56,0 ms | 51,3 / 56,4 ms |
| Frames > 33,3 / > 50 ms | 58,3 % / 8,2 % | 49,4 % / 5,6 % |
| Execute / DrawRec / Painter | 35,8 / 16,3 / 16,3 ms | 32,3 / 9,5 / 19,5 ms |
| Draws / merged por frame | 1450 / 761 | 1458 / 763 |
| Pipeline / uniform rebuilds por frame | 986 / 1363 | 989 / 1371 |

Memo: 1 743 410 aciertos, 0 fallos, 0 evicciones en la ventana (hit rate 1,0); máximo de entradas ocupadas 159 de 1024. Notas: el p99 es 0,4 ms mayor (dentro del ruido); Painter sube 3,3 ms, lo que sugiere que el hilo FIFO sigue siendo el límite (inferencia, no medición); con 159 entradas en uso, 256 bastarían (~0,75 MB en lugar de ~3 MB); no se hizo ejecución con el memo apagado en esa sesión.

## Decisiones (2026-10-01)

- H31-c1 queda **validado en hardware** (sin problemas visuales ni de estabilidad, confirmado por el usuario) y es la base de rendimiento actual; H30-c1 es el fallback. Resultados finales: los de la tabla anterior (una ejecución; 29,27 FPS, Execute 32,3 ms, DrawRec 9,5 ms, hit rate 1,0).
- El memo se conserva en **1024 entradas**. No se hace una variante de 256 y no se hace una prueba con el memo apagado.
- La SD conserva `dusklight.nro` (26/09): su SHA-256 `673c127b…0087` coincide con `SWITCH-TEST-H3/dusklight-nxvk.nro` (H3, backend NXVK). No se borra sin autorización del usuario. En la SD quedan `dusklight-h29-c1.nro`, `dusklight-h30-c1.nro` y `dusklight-h31-c1.nro`; `dusklight-h28-c1.nro` se borró.
- Siguiente hito: **H32**, dedicado exclusivamente al cierre de procedencia y a la eliminación o el reemplazo del código restante de souldbminer (`PROVENANCE.md`, `HITO6.md`). Sin optimizaciones mezcladas.
