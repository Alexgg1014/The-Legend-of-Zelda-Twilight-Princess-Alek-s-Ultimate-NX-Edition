# H33: resolución segura, perfiles por modo, recuperación predictiva y aspecto de Flip Grip

**Estado: H33-c1 (`H33-c1-52076ba037`) validado en hardware el 2 de octubre de 2026 (UTC), y nueva base.**
Ver «Resultado en hardware y cierre» al final. H32-c1-b84a849829 queda como respaldo; H31-c1 sigue más atrás.
Base de partida de este hito: H32-c1-b84a849829 (validada).

> El texto de abajo describe el diseño y el plan de prueba tal como se escribieron. Lo que se probó de verdad
> en consola, y lo que no, está en «Resultado en hardware y cierre».

Este hito parte del análisis estático del 2026-10-01 y de la revisión aprobada. Un solo candidato,
una sola sesión de validación. Dentro del alcance quedan:

1. Resolución segura (un resolvedor único, saneo de arranque, tope por modo, defensa en Aurora).
2. Perfiles Quality, Balanced y Performance con política de Auto-resolución recordada.
3. Gobernador de la interpolación predictiva (recuperación del estado `stalled`).
4. Aspecto efectivo de Flip Grip sin escribir la preferencia.
5. Benchmark con esquema 3 y `Run Profile Matrix`.

Fuera de alcance: selector de disco (pasa a **H34**), umbrales de interpolación por calidad,
benchmark multiárea, `tools/make-shader-seed.py` (deuda registrada para la fase de shaders), cross-save,
controles estilo Wii, deko3d y la publicación de V1. **No se hizo ninguna otra optimización de Aurora.**

## Commits

| Repositorio | Rama | Commit |
|---|---|---|
| `aurora-aleks-nx` | `claude/h33-aurora-window-fit` | `0020cbf6365c171e1f25988567e1d3a51168b7bc` (base `1d4c43ef…`) |
| principal | `claude/laughing-knuth-j353x7` | el commit que contiene este archivo (base `bf313a108…`); el gitlink `extern/aurora` apunta al de Aurora de arriba |

Aurora se publicó primero y el gitlink se actualizó después. Nada se fusionó a `main`.

## Diseño implementado

### 1. Resolución (`src/dusk/resolution_policy.hpp`)

Todos los caminos que cambian el framebuffer terminan en `applyInternalResolutionScale`
(`settings.cpp`), que llama a `respol::resolve`: arranque, callback del ajuste, perfil, dock/undock
(`display::poll`), `dynres` y configuraciones antiguas. Es el único sitio que llama a
`VISetFrameBufferHeight/Scale`.

Orden: (1) la resolución manual manda sobre Auto; (2) Auto usa `game.autoResolutionPolicy`;
(3) se aplica el tope del modo (portátil y Flip Grip 1280×720, acoplado 1920×1080); (4) Aurora
recorta otra vez (segunda barrera).

| Política | Acoplado | Portátil | Flip Grip |
|---|---|---|---|
| 0 Legacy (lo que era Auto) | 1080 | 720 | 540 |
| 1 Quality | 900 | 720 | 540 |
| 2 Balanced | 720 | 540 | 540 |
| 3 Performance | 720 | 540 | 540 |

Estos valores son **iniciales y sujetos a medición**: en el repositorio no hay ninguna medida acoplada a
900 ni a 720.

Saneo (cada arranque, dentro de `runConfigMigrations`, con un único `config::save()`):

| Caso | Valores | Acción |
|---|---|---|
| Corrupto (fuera del dominio) | fuera de 0–18 | Auto, WARN, aviso en pantalla, persistir una vez |
| Inseguro en Switch | factores 3–12, 17 y 18 (> 1080 filas) | Auto, WARN, aviso en pantalla, persistir una vez |
| Válido para acoplado, usado en portátil | 1080p, 900p, factor 2 | recorte solo en ejecución; no se reescribe |

La política de Auto desconocida (fuera de 0–3) se sanea a Legacy.

Carrusel de Switch: solo Auto, 540p, 720p, 900p y 1080p (`ui/graphics_tuner.cpp`). Corrige el paso
540p → ×12 del carrusel genérico (6336 filas), que es lo que causó el `VK_ERROR_OUT_OF_HOST_MEMORY`.
Muestra la selección guardada y la efectiva cuando difieren («1080p → 720p (handheld limit, …)»).

### 2. Perfiles (`perf_profile.hpp`, `perf_profile_apply.cpp`)

| Perfil | Interpolación | Sombras | Bloom / DoF |
|---|---|---|---|
| Quality | Off (30) | ×4 | Dusk / Dusk |
| Balanced | Capped 60 + Fancy + Predictive | ×2 | Dusk / Classic |
| Performance | Capped 60 + Fast + Predictive | ×1 | Classic / Off, sin refracción |

No hay bloqueo a 45 FPS. Elegir un perfil fija su política y la resolución Auto, y aplica su tabla.
Un cambio manual muestra «Custom» pero conserva la política; una resolución manual sustituye a Auto;
dock/undock no escribe configuración. La detección (`detectCurrent`) solo etiqueta la fila: **no decide
la resolución**.

Migración 4 (una vez): una configuración que coincide **exactamente** con el Quality, Balanced o
Performance antiguo pasa a la política de ese perfil con Auto (esto cambia el Auto acoplado del Quality
antiguo de 1080 a 900, intencionadamente); cualquier Custom, incluidos los Auto, queda en Legacy.

Campo nuevo: `game.autoResolutionPolicy`, añadido al final de la estructura `game`.

### 3. Gobernador predictivo (`interp_governor.hpp`, `game_clock.cpp`)

Causa del punto muerto: en `stalled` el limitador del tick (`JFWDisplay::waitForTick`) rellena cada
frame hasta 33,3 ms, y la regla de salida (intervalo < 30 ms) era inalcanzable por construcción.

- Entrada: intervalo (EMA) > 35 ms durante 30 frames consecutivos.
- Salida: **trabajo** (`intervalo − esperas`, EMA) < 30 ms durante 30 frames **y** cooldown cumplido.
  Mismo umbral para Fast y Fancy; no hay constantes por calidad.
- Cooldown 5 s; se duplica (5, 10, 20, 40, 60) si recae en menos de 15 s; vuelve a 5 s tras 30 s
  interpolando. **H36 lo cambia**: tope de 20 s, vuelta a 5 s tras 15 s estables, y una carga o pausa devuelve el
  cooldown inicial (`docs/switch-h36-mod-platform.md`, §10).
- Las esperas son el sueño real que miden el limitador del tick y el del bucle (`note_wait`). Se
  acumulan y `advance()` las consume en cada llamada: cada espera se atribuye al intervalo en que
  ocurrió. Se descartan junto con la muestra en `reset()` (pausa, cambio de tasa), cambio de escala de
  tiempo, espera predictiva y huecos ≥ 250 ms.
- Telemetría: cada transición se escribe en el log con calidad, `work`, `gap`, cooldown y contadores; el
  informe v3 trae el bloque `interpolation`.

### 4. Aspecto de Flip Grip (`dualscreen.cpp`)

Aspecto efectivo = `preferencia_guardada OR flip_activo` (`effectiveAspectLock`). Flip fuerza 4:3
mientras dura y al salir vuelve a la preferencia; **no se escribe `lockAspectRatio`**. Se eliminó la
escritura permanente del botón de layout y la de la migración 3. `applyAspectPolicy()` empuja la
política al viewport solo cuando cambia (arranque, cada frame desde `beginHudCapture`, `display::poll`,
la fila de ajustes y el preset). El táctil usa el mismo valor efectivo. Las configuraciones existentes
con `lockAspectRatio=true` no se tocan.

### 5. Aurora (commit `0020cbf`)

`lib/window_fit.hpp` (`fit_within`, enteros de 64 bits, solo biblioteca estándar) y una llamada en
`get_window_size` bajo `__SWITCH__`, **después** del ajuste de aspecto y antes de construir
`AuroraWindowSize`, con los máximos nativos del modo actual. Reduce proporcionalmente (eje limitante,
redondeo al más cercano), mínimo 1×1, idempotente, no toca lo que ya cabe. Un WARN por episodio de recorte.
Un WARN de Aurora en el log significa que el filtro del repositorio principal dejó pasar algo.
Prueba: `tests/window_fit_test.cpp`. Otras plataformas, sin cambios.

### 6. Benchmark (esquema 3)

`Run Benchmark` conserva su comportamiento y F_SP122; no cambia ningún ajuste. El informe añade:
`settings.profile` (+ `profile_source`), `auto_resolution_policy`, nombre de la interpolación y
calidad, aspecto guardado y efectivo, `device.docked` y `flip_active`, `resolution.setting_saved`,
`effective_rows`, `cap_rows`, `clamped`, `repaired`; `frames.judder_pairs` y `judder_pct`; el bloque
`timing` (espera del tick, del limitador y `work_ms_per_frame`); el bloque `interpolation`; y la cola de
pipelines (frames con cola, media). **`Begin` y `Present` se publican como proxies**, no como tiempos de
GPU ni de presentación: `Begin` = espera de slot de frame + ImGui; `Present` = drenaje del FIFO +
grabación. La espera de vsync ocurre en el hilo de render y no es visible desde el bucle principal.
`gpu.available` sigue en `false`. La zona `VIWait` (que no esperaba nada) se renombró `VIStub`; la nueva
`TickWait` mide el limitador del tick.

## `Run Profile Matrix`: **entró** (con las condiciones que se indican)

Botón aparte en Diagnostics. Mide Quality, Balanced y Performance en el modo actual, un informe por perfil.

Auditoría de persistencia (`test-config-override-audit.sh`, sobre el `config_var.hpp` real):

- Un override **nunca** es lo que devuelve `getValueForSave()`: no llega a `config.json` aunque algo
  llame a `config::save()` durante la matriz.
- `clearOverride()` devuelve el valor guardado a una variable en la capa Value.
- **Peligro documentado:** en una variable aún en la capa Default, `clearOverride()` deja `value`
  sin inicializar (0), no el valor por defecto. Por eso la matriz **se niega** si necesita cambiar una
  variable que no esté en la capa Value (o que esté en Override/Speedrun por argumento o modo speedrun),
  con el motivo visible. Una variable que ya tiene el valor objetivo no se toca.
- No se amplió la infraestructura de `ConfigVar`: solo se usa `getLayer()`, `setOverrideValue()` y
  `clearOverride()`, que ya existían.

Comportamiento: no toca vsync, no persiste nada, mantiene el bloqueo de guardado, se cancela con Start,
con el menú o si cambia el modo (dock/Flip), y siempre devuelve los ajustes (`begin_restore` →
`clearOverrides`, y de nuevo en `end_run`). Un fallo del proceso los pierde por diseño (viven en memoria).
Si la matriz se negara por la capa de una variable, el plan B son corridas manuales: el informe ya
registra el perfil.

## Pruebas host (todas pasan)

`test-resolution-policy.sh`, `test-perf-profile.sh`, `test-interp-governor.sh`,
`test-config-override-audit.sh`, `test-benchmark-report.sh` (incluye judder y esquema 3), más las
existentes: `test-pipeline-memo.sh`, `test-switch-patches.sh`, `test-profiler-ladder.sh`,
`test-disc-header.sh` (sin imágenes de referencia). En Aurora: `tests/window_fit_test.cpp`.
Los valores 12 y 99 se prueban exhaustivamente en host.

Lo que **no** pudo comprobarse en el entorno de nube: la compilación del NRO y de `settings.cpp`,
`ui/*`, `dualscreen.cpp` y `benchmark.cpp` (necesitan RmlUi y el SDK de Switch; solo se verificó la
sintaxis de `game_clock`, `dynres`, `switch_display`, `perf_profile_apply` con y sin `__SWITCH__`). El
verificador `check-mod-abi.py` **no existe en este repositorio** (se citaba desde el espacio de trabajo
original), así que no se ejecutó; la estructura `UserSettings` no aparece en las cabeceras del SDK de mods
(`sdk/include`) y el campo nuevo va al final de la estructura.

## Compilar H33-c1 en local

```
git fetch origin claude/laughing-knuth-j353x7 && git checkout claude/laughing-knuth-j353x7
git submodule update --init            # extern/aurora queda en 0020cbf6…
# el árbol del repositorio debe estar limpio para que BuildIdentity registre dirty=0
cmake --preset switch-devkitA64-release \
  -DALEKS2_STATIC_MODS="template_mod;flow_demo;rollandsprint" \
  -DNAGA_VULKAN_ROOT=<ruta>/mesa-naga-2623/opt/devkitpro/portlibs/switch \
  -DALEKS2_BUILD_TAG=H33-c1
cmake --build --preset switch-devkitA64-release
```
`BuildIdentity.cmake` lee el commit al configurar: configurar **después** de hacer checkout del commit.

## Prueba única en Switch (≈ 60 min)

Baseline H32 (F_SP122, Flip Grip, 540p, Quality, memo con 0 misses): 29,52 FPS; p95 48,2 ms;
1 % low 18,0 FPS; Execute 31,9 ms; DrawRec 8,97 ms. Precondición: confirmar en el JSON de H32 el perfil e
interpolación con que se corrió (no está en el repositorio).

1. **Config ×12:** copia de seguridad de `config.json`; poner `game.internalResolutionScale` a 12 y
   arrancar. Debe arrancar, reparar a Auto, mostrar el aviso y dejar el WARN en el log. (99 solo en host.)
2. **Aspecto:** con `lockAspectRatio=false` alternar Flip (4:3 y vuelta a 16:9); con `true`, entrar y salir
   (sigue `true`); arrancar directo en Flip; dock y undock en Flip. El `config.json` no debe cambiar.
3. **Perfiles:** elegir un perfil una vez (para que los valores queden guardados); `Run Profile Matrix`
   en portátil y en acoplado, y una corrida manual de Quality en Flip comparable con H32. Hash del
   `config.json` antes y después: idéntico. Opcional: Quality acoplado a 1080p manual frente a 900.
   Si la matriz se niega, hacerlo con corridas manuales.
4. **Recuperación:** con Performance (Fast) y con Balanced (Fancy): zona pesada (estado `stalled` en el
   log) → zona ligera (debe reanudar sola) → pesada.
5. **Tope por modo:** 1080p manual acoplado → desacoplar (efectiva 720, valor guardado intacto, carrusel
   muestra ambas) → reacoplar.
6. **Resistencia:** ≥ 10 ciclos dock/undock y ≥ 10 cambios de perfil, sin crash, OOM ni fuga apreciable.

Aceptación: Quality/Flip/540p frente a H32 con FPS ±3 %, p95 ≤ 50 ms, Execute ±1,5 ms, DrawRec ±1 ms,
memo sin misses; ningún tamaño efectivo por encima de 1280×720 (portátil) ni 1920×1080 (acoplado) y
ningún WARN de Aurora por recorte; el gobernador reanuda sin tocar la opción, ≤ 6 transiciones/min y
`resume_failures` ≤ 2 por sesión; `lockAspectRatio` intacto; log sin ERROR. Los FPS de Balanced y
Performance son evidencia para calibrar, no criterio de aceptación.

Cancelan H33-c1 (en lugar de un c2 inmediato): VK OOM o pérdida de dispositivo con resoluciones legales,
tamaño por encima del tope, bucle de arranque o configuración corrupta tras la reparación, aspecto
atascado tras salir de Flip, gobernador atascado con otra causa o con oscilación descontrolada,
regresión de Quality > 5 % frente a H32, informe que no distingue modo o perfil.

## Resultado en hardware y cierre

**Candidato probado:** `H33-c1-52076ba037` (commit principal `52076ba037bfcded57f2b52c28e8fe518537d935`,
Aurora `0020cbf6365c171e1f25988567e1d3a51168b7bc`), compilado desde un árbol limpio, Mesa 26.2.3 reparado,
mods estáticos `template_mod`, `flow_demo`, `rollandsprint`.

| | |
|---|---|
| NRO | `dusklight-h33-c1.nro`, 59 763 111 B |
| SHA-256 del NRO | `90845c2e0f5a3f203c422bf07db2b0d1af45a46c3e84fb41beba1aa6dc88b1a2` |
| `verify-nro.py` | «NRO built from this ELF»; text, rodata y data idénticos |

Validación local antes de la consola: auditoría del diff sin hallazgos; ABI contra el upstream `v2.0.3` correcta
(con una copia temporal del verificador apuntada a este árbol, más un control positivo y uno negativo); todas
las pruebas host, incluida `window_fit` de Aurora, y `git diff --check` limpios.

### Prueba de humo: aprobada

Una sesión de unos 960 s en portátil y acoplada, arrancando desde el RVZ. Sin crash ni errores visuales importantes.

- **Disco:** `Loading DVD image: …GZ2P01-test.rvz` y `Loaded game disc is GZ2P01`.
- **Migración 4:** `configMigration` pasó de 3 a 4. El config era exactamente el Quality antiguo, así que
  arrancó con `policy=1`; terminó en `autoResolutionPolicy: 2` (el último perfil elegido, Balanced).
- **Perfiles y resolución efectiva** (Auto, según el log de sesión y los relojes de GPU):

| Perfil | Ajustes | Portátil | Acoplado | Flip Grip |
|---|---|---|---|---|
| Quality | Off/Fancy, sombras x4, bloom y DoF Dusk | 720 | 900 | — |
| Balanced | Capped 60 + Fancy, predictive, x2, Dusk/Classic | 540 | 720 | 540 |
| Performance | Capped 60 + Fast, predictive, x1, Classic/Off, sin refracción | 540 | 720 | — |

- **Dock/undock:** al menos cuatro cambios de modo (dos acoples y dos desacoples, según los relojes de GPU del
  log); la resolución se ajustó bien en cada uno. Balanced acoplado usó 720p
  de verdad (`res=0(720 rows)`, GPU 768 MHz).
- **Flip Grip:** entrada y salida sin problema; `video.lockAspectRatio` siguió en `false` antes y después, así que
  no cambió por entrar o salir. El 4:3 efectivo no se registra en el log; solo se observó a la vista.
- **Errores:** ninguno (sin ERROR ni FATAL; ni Vulkan, memoria, audio, controles o disco). Hubo **un** WARN de
  Aurora, `Framebuffer 1600x900 exceeds the 1280x720 display limit`, entre ~796 y ~810 s, justo al desacoplar con
  Quality acoplado a 900p: el único cambio donde la resolución acoplada supera el límite de portátil. La
  segunda barrera lo recortó proporcionalmente, sin OOM, y el latido siguiente ya marca 720 filas. Se interpreta
  como un desfase transitorio de una transición (Aurora lee el modo del applet en cada llamada y el filtro principal
  lo reaplica una vez por frame); no se midió su duración.
- **Gobernador:** 17 entradas a `stalled`, 13 recuperaciones automáticas y 9 recaídas. Ya no hay punto muerto: se
  recupera sin apagar y encender la opción.

### Profile Matrix acoplada: aprobada

Una ejecución en F_SP122, acoplada, tres informes válidos (esquema 3, `completed`). Cero WARN/ERROR en el log
(132 líneas INFO). Cola de pipelines 0 y ningún pipeline creado en las ventanas; el memo tuvo 0 misses en Quality y
Performance y 3 en Balanced, con 157–164 de 1024 entradas ocupadas.

| | Quality | Balanced | Performance |
|---|---|---|---|
| Resolución efectiva | 1600×900 | 1280×720 | 1280×720 |
| FPS medio / 1 % low | 29,32 / 20,65 | 29,00 / 19,73 | 29,61 / 21,28 |
| p95 / p99 (ms) | 45,6 / 47,6 | 46,4 / 49,0 | 44,7 / 46,1 |
| Trabajo real por frame (ms) | 28,84 | 29,44 | 27,67 |
| Espera del tick (ms) | 5,26 | 5,04 | 6,09 |
| Execute / DrawRec (ms) | 32,1 / 11,2 | 31,8 / 11,1 | 31,6 / 10,1 |
| Entradas / recuperaciones / recaídas | sin gobernador | 2 / 2 / 2 | 1 / 1 / 1 |
| Tiempo en stalled | — | 57,5 s (95,7 %) | 58,8 s (98 %) |

- **`config.json` restaurado de forma idéntica:** el config tras la matriz tiene el mismo hash (`694ccdbc…`) que el
  de después de la prueba de humo. Un único «Saving config» en toda la sesión, anterior a la matriz: no escribió nada.
- **Aurora:** sin aviso de clamp durante el modo acoplado estable. Esa ejecución no incluye ningún undock, así que no
  confirma ni refuta la lectura de transición del WARN anterior.
- **Quality:** estable cerca de 29,3 FPS, sin frames de más de 50 ms; comparable al baseline de H32 (29,52 FPS,
  1 % low 18,0 y p95 48,2 ms).

### Lectura de los resultados

- **F_SP122 no sirve para medir el techo de 45 o 60 FPS.** En esa escena el trabajo plano ronda 27–29 ms (cerca
  del tick de 33,3 ms), así que Balanced y Performance caen correctamente al fallback seguro de 30 (stalled el
  96–98 % del tiempo). Sus FPS no deben usarse para decidir si Balanced llega a 45 ni si Performance llega a 60.
  Los objetivos de 45/60 se medirán después en una zona media reproducible.
- **Balanced frente a Performance:** Performance trabaja unos **1,8 ms menos por frame** (1,0 ms menos en DrawRec),
  compatible con sombras x1, bloom Classic, sin DoF y sin refracción. Esta corrida no separa Fancy de Fast, porque
  casi todo el tiempo estuvieron en stalled y se midieron frames planos. Execute es ~32 ms en los tres perfiles,
  aunque Quality renderiza a 900p con sombras x4: en esta escena el límite es el coste CPU/FIFO, no la resolución
  ni las sombras.
- **El umbral de reanudación actual es demasiado optimista en F_SP122.** Las 5 reanudaciones de la matriz recayeron
  (reanudan con trabajo de 26,4–28,0 ms, por debajo de los 30 ms, y recaen con 39–44 ms de intervalo). Con
  interpolación hacen falta dos frames por tick de 33,3 ms. El cooldown creciente (5, 10, 20, 40, 60 s) evita una
  oscilación continua y acota el coste a ~1,2–1,3 s por intento. El criterio de «≤ 2 recaídas por sesión» de la
  prueba única no se cumple en este peor caso.
- Se registra como **deuda de tuning posterior**, no como motivo para un H33-c2: no hay crash, corrupción, tamaño
  ilegal ni cambio de configuración.

### Lo que no se probó directamente en consola

- **La reparación de un `game.internalResolutionScale` de x12** (o fuera de dominio): solo se probó en pruebas
  host. No se probó en consola el arranque con ese valor, el aviso «Settings repaired» ni su WARN de log. El
  crash por x12 de una ejecución anterior de H32 fue un accidente y no es evidencia de esta reparación.
- Un 1080p manual en portátil (el texto «1080p → 720p (handheld limit…)» y el clamp con valor guardado intacto).
- La repetición de ≥ 10 ciclos de dock/undock y ≥ 10 cambios de perfil.
- El resto de los puntos del plan de «Prueba única en Switch» que no aparecen arriba.

### Estado

**H33-c1 queda validado en hardware y pasa a ser la nueva base. H32-c1-b84a849829 queda como respaldo.**
La base de código para el siguiente hito es el commit que contiene este documento en la rama
`claude/laughing-knuth-j353x7`.

## Deuda y seguimiento

- **H34:** selector de disco (spec en el análisis: explorador sobre `borealis_file_select_switch.cpp`,
  validación de cabecera antes de guardar `isoPath`, etiquetas ISO/RVZ-zstd validados y WIA/GCZ
  experimentales, recuperación sin `fatal`).
- **Tuning del gobernador (deuda de H33-c1, no bloquea):** el umbral de reanudación (trabajo < 30 ms) es demasiado
  optimista en escenas pesadas; las recuperaciones recaen y solo el cooldown evita la oscilación. Revisarlo, y
  decidir si va por calidad (Fast/Fancy), con medidas de una zona media reproducible.
- **Objetivos de 45/60 FPS:** medirlos en una zona media reproducible, no en F_SP122.
- Umbral de salida por calidad (Fast/Fancy): decidir con los JSON de H33.
- `tools/make-shader-seed.py`: ausente; deuda para la fase de shaders.
- Verificador de ABI de mods: recuperar o reescribir en este repositorio.
