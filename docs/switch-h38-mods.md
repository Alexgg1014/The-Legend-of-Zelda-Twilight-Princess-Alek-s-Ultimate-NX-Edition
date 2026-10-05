# H38 — teclado, ciclo de vida de Randomizer, controles Wii y mods `switch-arm64`

Base: H37-c8 (`658d23b`) + el arreglo de entropía de Randomizer (`14fce14`). H38 añade seis cosas; esta página
dice qué hace cada una, qué prueba la cubre en PC y qué solo se puede ver en la consola.

## 1. Teclado nativo (`PlatformTextInput`)

`src/dusk/ui/platform_text_input.hpp` es la costura entre un `BaseStringButton` y el teclado de la plataforma.
El backend por defecto devuelve `is_native() == false` y el campo conserva su editor RmlUi (PC, Android, iOS sin
cambios). En Switch, `platforms/switch/source/switch_text_input.cpp` abre `swkbd` de libnx: QWERTY para texto, pad
numérico para campos `number`.

* Un `BaseStringButton` pide la edición (`NativeTextEditSession::request`) y el teclado se abre **una vez**, desde
  `update()`, nunca por frame ni con un `<input>` oculto.
* Longitud: `maxLength` caracteres; sin límite (o < 1) 256 bytes UTF-8. El resultado se recorta sin partir un
  carácter.
* Aceptar aplica el valor una vez por el mismo `set_value` del editor actual; cancelar o error conservan el valor;
  al cerrar se restaura el foco (`set_selected(false)`, `mRoot->Focus(true)`).
* Prueba: `test-text-input.sh` (aceptar, cancelar, error, límites, llamada única, foco, con un backend falso).
* No verificable en PC: `swkbdShow` real (y que cancelar devuelva un `Result` de fallo; el juego trata cancelar y
  error igual: el valor no cambia).

## 2. Ciclo de vida de Randomizer (parche `0009`)

El submódulo sigue fijado en `1f24e81`. `platforms/switch/patches/randomizer/0009-idempotent-game-mode-lifecycle.patch`
añade `src/session_lifecycle.hpp`: una máquina `Inactive / Activating / Active / ShuttingDown` con los recursos en
orden de adquisición — hooks, observador de guardado, textura, UI, estado de semilla, hilo de precarga — y liberación
en orden inverso.

* Una activación que falla a medias deshace lo adquirido; `shutdown()` libera cada recurso exactamente una vez, se
  llame las veces que se llame (`onGameModeDeactivated()` y luego `mod_shutdown()` es seguro).
* Los handles se invalidan al liberarse (`release_handle`): `removeMenuTab()` ya no produce
  `unregister_menu_tab: stale handle`; un `ui::initialize` fallido libera sus estilos.
* El hilo de precarga ya no es un hilo suelto: `LargeStackThread` con bandera de cancelación, y `join` antes de
  desmontar semilla, UI y servicios.
* Prueba: `test-randomizer-lifecycle.sh` aplica la serie real de parches a la fuente fijada y ejecuta la lógica
  parcheada con servicios falsos con contadores (también con ASan/UBSan y TSan a mano). `test-randomizer-entropy.sh`
  sigue pasando.
* No verificable en PC: el orden real dentro del juego (generar, guardar, cargar, descargar el mod en consola).

## 3. Controles de movimiento estilo Wii

Original: sacudir el Wii Remote = espada; mover el Nunchuk de lado = ataque giratorio; empujarlo hacia delante =
escudo / shield bash (`m_Re_controller_pad.cpp`, `Pad::Acc::calcSttsRem/Fs`). En Switch, con dos Joy-Con
separados: derecho = Wii Remote, izquierdo = Nunchuk. Con un solo mando (Pro, portátil, un Joy-Con) solo la sacudida
(espada); los gestos del Nunchuk se muestran como no disponibles.

* `aurora::device::SensorSide {Primary, Left, Right}` (Aurora `cf9b2d1`, rama `claude/h38-development` de
  `aurora-aleks-nx`): `Primary` es el sensor de siempre; `Left`/`Right` solo existen en un par desacoplado. Las
  funciones previas son envoltorios de `Primary`.
* `src/dusk/wii_gesture_detector.hpp` (puro): quita la gravedad con un paso bajo, compara la magnitud de la
  aceleración dinámica con un umbral (≈1,3 g, como el original) con histéresis y 0,25 s de bloqueo; un golpe
  (acelera y frena) da un solo pulso. `src/dusk/wii_controls.cpp` muestrea una vez por tick, **antes** de
  `mDoCPd_c::read()`, y reparte:
  * espada → pulsación B ordinaria en el pad 0 (tras `convert()`, antes de `benchmark::filter_pad`);
  * giro → `consume_spin()` dentro de `daAlink_c::checkCutTurnInputTrigger` (las validaciones vanilla siguen mandando);
  * shield bash → `consume_shield_bash()` junto a `spActionTrigger()` dentro del bloque ya validado (habilidad, escudo,
    suelo): el gesto no concede la técnica antes de desbloquearla.
* Bloqueado con un documento de Dusklight abierto, en benchmark/tour, sin gameplay (sin Link, evento, menú o mapa),
  en pausa y durante el recentrado; filtros y bloqueos se reinician al cambiar de mando, perder sensores, pausar o
  desactivar. Ajustes (Off por defecto): *Wii-style Motion Controls*, *Gesture Sensitivity*, *Recenter Motion
  Sensors* y un texto de estado con los sensores Right/Left detectados y los gestos disponibles.
* Telemetría: solo cambios de estado y un resumen cada 25 gestos; nada por frame.
* **Límite**: el apuntado por movimiento es el Gyro Aim existente; no hay cursor Wii para menús.
* Pruebas: `test-wii-gestures.sh` (reposo y ruido, un pulso por gesto, sin repetición en el bloqueo, dos gestos
  separados, bloqueos, reinicios, un solo sensor nunca da Nunchuk).
* **Por confirmar en consola**: los ejes de cada Joy-Con sostenido como Nunchuk (`SideAxes`, §3.1: `config.json` los corrige sin
  recompilar) y los umbrales (`game.wiiAccelThresholdG`, etc.).

### 3.1 H38-c3: gestos con ventana completa, direcciones, mano de la espada

Tras la prueba de H38-c2 (se perdía ~1 de cada 3 golpes y todos daban el mismo ataque):

* **Causa de las pérdidas:** el detector se desarmaba tras cada golpe hasta que `|dyn| < 0.52 g` frente a una gravedad que se adaptaba 5×
  más lenta mientras había movimiento; un giro de muñeca de ≈45° la dejaba obsoleta durante segundos (reproducido en host: 4 de 6 golpes).
  Las unidades de Aurora eran correctas (g→m/s², rev/s→rad/s).
* **Detector (`wii_gesture_detector.hpp`):** gravedad rotada con el giroscopio (filtro complementario; la lateralidad del giroscopio se
  deduce de los datos y, mientras no se sepa, no se usa y no se llama izquierda/derecha); una **ventana** de 60–120 ms desde que la
  aceleración dinámica (`accelThresholdG`) o la velocidad angular (`gyroThresholdRad`) cruzan su umbral; clasificación por **energía por eje**
  (no por la integral neta, que se cancela al acelerar y frenar) y por el giro alrededor de la vertical: `Vertical`, `LeftToRight`,
  `RightToLeft`, `Generic` (fuerte pero ambiguo: ataca igual) o rechazo (`short`, `rotation-only` —apuntar—, `gap`). Rearme sin depender
  de la inclinación (energía quieta + enfriamiento; si la gravedad quedó obsoleta se rebasa desde el acelerómetro).
* **Aurora:** `aurora::device::motion_samples()` (aceleración + giroscopio + tiempo real de cada muestra, en `lib/motion_queue.hpp`) es el único
  lector de la cola de 17 estados; un hueco, un desbordamiento, un reinicio del contador o un estado desconectado marca la muestra siguiente
  (`kMotionGapBefore`) y la ventana que lo cruza no es un ataque. `delta_time` no se da por nanosegundos: se acepta como periodo plausible
  (1–40 ms) en ns o µs y, si no, se usa la cadencia del `sampling_number`; el log dice el rango real una vez.
* **Animaciones:** el pulso B se inyecta como antes y el gesto se guarda aparte con identidad (serial) en una ranura de un solo uso
  (`wii_cut_select.hpp`). `daAlink_c::checkCutAction()` lo toma al entrar (una vez) y solo en los cortes normales (combo 1–3, con o sin objetivo,
  y el siguiente golpe de una cadena) elige `CUT_NM_PARAM_VERTICAL/LEFT/RIGHT`. Remates (combo 4), golpe a enemigo aturdido, saltos, cargados,
  giro, caballo, lobo y demás conservan su animación y **descartan** el gesto: `commonProcInit()` lo descarta en cualquier cambio de
  procedimiento salvo la carga del corte, y una pulsación física de B también. No se simula el stick.
* **Lados:** en c3, `LeftToRight → RIGHT`, `RightToLeft → LEFT`; se intercambian si exactamente uno de *Mirror Mode* y *Swap Swing Sides* está
  activo (XOR explícito). La prueba en consola de c3 mostró que ese mapeo base estaba al revés (hubo que activar *Swap Swing Sides*): **c4 lo
  corrige, ver 3.2**.
* **Ajustes en la UI:** *Motion Sword Hand* (Joy-Con derecho/izquierdo; el otro es el Nunchuk; con un solo mando no cambia nada),
  *Swing Direction Animations*, *Swap Swing Sides*. No espejan nada.
* **Ajustes solo en `config.json`** (claves bajo `game.`; si faltan o están fuera de rango se usa el valor por defecto y se registra una vez):
  `wiiAccelThresholdG` 0.6–3.0 (1.3), `wiiGyroThresholdRad` 1.0–12 (3.0), `wiiDirectionDominance` 1.1–3.0 (1.5), `wiiMinYawRad` 0.05–1.5 (0.25);
  por Joy-Con físico como Nunchuk: `wiiAxisLateralLeft/Right`, `wiiAxisForwardLeft/Right` (-1 = tabla; 0–2 = eje x, y, z) y
  `wiiForwardSignLeft/Right` (-1, 0, 1; 0 = cualquier sentido). Los valores efectivos salen en una línea `wii motion tuning:` al activar.
* **Telemetría resumida:** una línea por ventana (aceptada o rechazada, máx. 4/s), el movimiento más fuerte que no abrió ventana, las muestras
  perdidas y un resumen cada 25 gestos con la ranura (ofrecidos/usados/descartados/caducados). Nunca por muestra ni por frame.
* **Pruebas:** `test-wii-gestures.sh` (golpes simulados con aceleración/frenado/rebote desde cinco poses y las dos lateralidades del giroscopio;
  sin pérdidas con giros de muñeca de 45–90°; sin ataques fantasma al apuntar, caminar, vibrar o girar sin mover la mano; un ataque por
  oscilación; huecos; temporización; Nunchuk por lado; mano de la espada; un solo sensor; reinicios; rangos), `test-wii-cut-select.sh`
  (tabla gesto × mirror × swap × rama; ranura de un solo uso; descarte), `test-motion-queue.sh` (cola de Aurora).
* **Fuera de H38-c3 y H38-c4:** Link visualmente diestro sin reflejar el mundo (ver 3.3).

### 3.2 H38-c4: lados de la espada, atajo de recentrado y versión

Prueba de c3 en consola (log `dusklight-20261004-170904.log`): los gestos funcionan y no hubo ataques fantasma; se registraron **304 gestos de
espada** en todo el log (278 con el Joy-Con derecho: 49 verticales, 61 izquierda→derecha, 61 derecha→izquierda, 107 genéricos; 26 con el
izquierdo), 34 giros y 56 escudos. El resumen intermedio del propio log (285 espadazos) era una instantánea a mitad de sesión, no el total.
c4 **no cambia umbrales ni clasificación**.

* **Lados (corrección):** con *Mirror Mode* y *Swap Swing Sides* apagados, un trazo de la mano de izquierda a derecha reproduce el corte
  `CUT_NM_PARAM_LEFT` y de derecha a izquierda `CUT_NM_PARAM_RIGHT` (el juego los nombra por el lado desde el que sale la hoja). Es la misma
  tabla para cualquier mano de la espada: el detector ya informa el trazo tal como lo movió la mano y esta capa solo lo traduce a los enums
  del juego. *Swap Swing Sides* sigue siendo un ajuste manual y se combina con *Mirror Mode* por XOR. Quien activó *Swap Swing Sides* en
  c3 debe **desactivarlo** en c4 (su valor guardado ya no es necesario).
* **Mirror Mode y la mano de Link:** la versión de GameCube que ejecuta Dusklight muestra a Link **zurdo** (espada en la izquierda, escudo en
  la derecha). **Mirror Mode** refleja el mundo y hace que Link se vea **diestro**, como en la versión de Wii; sin Mirror Mode se conserva el
  Link zurdo de GameCube. El texto del ajuste lo dice. *Motion Sword Hand* solo elige qué Joy-Con da el golpe: no cambia lo que se ve.
* **Atajo de recentrado:** mantener **L3 + R3 durante 0.5 s** (solo con *Wii-style Motion* activado). Una activación por cada pulsación
  mantenida; no se repite hasta soltar **ambos** sticks; un solo stick no cuenta, y si se suelta uno el tiempo se reinicia; una pausa larga del
  juego cuenta como máximo 0.1 s por tick; al activar el movimiento con los sticks ya pulsados no dispara hasta soltarlos una vez. Al
  activarse hace lo mismo que el botón de Ajustes (descarta ventanas y gestos pendientes y retiene los gestos 0.6 s), muestra el aviso
  «Motion sensors recentered» 2 s y da una vibración corta (`aurora::device::rumble`, 40 ms). El botón *Recenter Motion Sensors* de Ajustes
  se conserva. El juego no usa L3 ni R3 (nada lee `PAD_BUTTON_LEFT/RIGHT_STICK`) y el atajo no los consume; `ZL + R3`
(compañero en modo Normal) no lo activa porque exige L3 a la vez. Código: `wii_recenter_combo.hpp` (puro, probado en host),
  `aurora::device::stick_clicks()` (libnx `padGetButtons` en Switch, SDL en otras plataformas) y `wii_controls.cpp`.
* **Versión de la aplicación:** hasta c3 el menú mostraba `UNKNOWN-VERSION` y el NACP `0.0.0.0` porque este checkout no tiene tags `v*`.
  `cmake/DuskAppVersion.cmake` resuelve, en este orden: `DUSK_APP_VERSION_OVERRIDE` (el preset H38 lo fija a `v1.0.0-beta.1`; solo `v1.0.0` o
  `v1.0.0-beta.1`), un tag de versión de git, o el Build ID (`<ALEKS2_BUILD_TAG>-<commit>`) con el número `1.0.0.0`. Resultado en c4: menú
  `1.0.0-beta.1` (el menú quita la `v`), versión numérica `1.0.0.0`, NACP `1.0.0-beta.1`. El gate rechaza un NRO con la versión
  `UNKNOWN-VERSION`, vacía o de cero (`0`, `0.0.0`, `0.0.0.0`), sin NACP, o con la cadena `UNKNOWN-VERSION` en la imagen
  (`h38_policy.version_rejections`). Pruebas: `test-app-version.sh` (override beta, tag estable, tag con distancia, tag sucio, fallback al Build ID,
  versión cero, overrides inválidos; el gate con NACPs sintéticos).
* **Pruebas de host de c4:** `test-wii-cut-select.sh` (mapeo corregido y XOR), `test-wii-recenter-combo.sh`, `test-app-version.sh`, y las de c3
  (`test-wii-gestures.sh`, `test-motion-queue.sh`) sin cambios.

### 3.3 Link visualmente diestro (no implementado)

Un Link diestro **sin reflejar el mundo** exige cambiar cómo se ve Link, no solo qué sensor ataca. La ruta (`switch-v1-roadmap.md`, «Ruta
confirmada con Dawnlight Dual Wield») es adaptar la técnica de Dawnlight (CC0): un envoltorio de `J3DAnmTransformKey` que, durante el cálculo
del cuerpo, intercambia los huesos de ambos brazos y refleja localmente el torso, y colocar espada, estela y puntos de golpe en la mano opuesta.
Dawnlight no demuestra el escudo en la otra mano. Candidato a H39, empezando por un prototipo de laboratorio con un solo ataque normal.

### 3.4 H38-c5: selector Play / Randomizer y cinco paquetes oficiales opcionales

* **Selector del menú principal.** Con Randomizer activo el selector tiene dos modos. `src/dusk/ui/input.cpp` repite LEFT/RIGHT mientras se
  mantienen (a los 0.32 s, luego cada 0.12 → 0.045 s) y `GameModeButton` ciclaba en cada `Keydown`: dos eventos daban `Play → Randomizer → Play`
  y parecía que no se movía. El toque en las flechas es un solo click y siempre funcionó. `GameModeCycleLatch`
  (`src/dusk/ui/game_mode_cycle_latch.hpp`) acepta el primer `Keydown` de cada sentido, ignora las repeticiones hasta el `Keyup` y se rearma;
  izquierda y derecha son independientes; se reinicia al perder el foco el botón (el `Keyup` iría a otro sitio). El autorrepetido global no cambia.
  Prueba de host: `test-game-mode-cycle-latch.sh` (y `.ps1` en Windows).
* **Cinco paquetes oficiales, opcionales, solo exports.** Todos sus imports faltantes ya existían como funciones reales en el ELF de c4; no hay
  wrappers ni shims. Los requisitos salen de los paquetes exactos (por SHA-256) con `gen_required_symbols.py --merge`, sin retirar los de otros mods:

  | Paquete (id) | Versión | Archivo en `packages\` | Imports | Hooks | Páginas |
  |---|---|---|---:|---|---:|
  | Cowboy Link (`alexandre.cooper_hat.texture_selector`) | 1.1.0 | `cowboy_link.dusk` | 37 | ninguno | 0 |
  | Deferred Fog (`dev.automata.deferred_fog`) | 2.0.0 | `deferred_fog.dusk` | 48 | 3 por función, 8 por miembro | 11 |
  | Fairy Companion (`com.agustinfuac.fairy_companion`) | 1.0.0 | `fairy_companion.dusk` | 52 | ninguno | 0 |
  | KH2 Camera (`com.kite.kh2camera`) | 1.0.1 | `kh2_camera.dusk` | 113 | 1 por nombre, 1 por función, 7 por miembro | 9 |
  | TP Classic Modern Controller UI (`org.dusklight.tp_classic_buttons`) | 1.3.1 | `tp_classic_buttons.dusk` | 47 | 13 por miembro | 13 |

  En el gate (`OPTIONAL_H38`, solo perfil h38) son **optional**: ausente no bloquea; presente e incompatible bloquea; presente y compatible entra en
  los perfiles de capacidad. Se usan en modo compatibilidad (`lib/linux-aarch64/mod.so`): ninguno tiene TLS, TLSDESC, blockers ABI, imports denegados
  ni servicios imposibles.
* **Capacidad.** Conjunto principal de c4 (Cosmetics + Roll and Sprint + Randomizer + Twilit) = 467 páginas; los cinco = 33; **todos presentes =
  500 de 512 páginas, 8 objetos**; el límite no sube. Quedan **12 páginas**: no se puede prometer que otro mod con hooks quepa junto a los cinco.
  El gate informa `remaining_pages` y lo avisa cuando quedan menos de 64.
* **Matriz de compatibilidad (estado tras c5; sustituida por la de 3.5 tras la prueba de hardware de c5).**
  * *Probados en consola:* Randomizer 1.0.6 y Twilit Essentials 2.5.0 (variantes `switch-arm64` de H38), Cosmetics, Roll and Sprint, runtime Luau.
  * *Compatibles estáticamente en c5, pendientes de hardware:* los cinco de la tabla. **En consola**: Cowboy Link funcionó, Deferred Fog quedó
    inactivo y TP Classic UI 1.3.1 crasheó (3.5).
  * *Incompatibles:* Dawnlight 3.8.1 (TLS/TLSDESC, ABI de iostream, capacidad) y ALBW 0.3.0 del catálogo (ABI de iostream; la variante Switch sigue
    pendiente por `dlsym`).
  * *Cualquier otro mod:* no probado; no se etiqueta como incompatible sin una auditoría (`H38-CATALOG-COMPATIBILITY-AUDIT.md`).
  Chicken Bombs, Cutscene Player, Zora Swim, Zelda on Epona, Hyrule Hide & Seek y Dark Morpheel quedan fuera de c5 (exigen retención, runtime o hooks).

### 3.5 H38-c6: lo que mostró la consola, separado de lo estático

La prueba de hardware de c5 (`SWITCH-TEST-H38-C5\logs-consola\DIAGNOSTICO-H38-C5-2026-10-04.md`) demostró que **READY del gate no certifica el
comportamiento de los mods del catálogo**: certifica la candidata y sus requisitos obligatorios. Cowboy Link 1.1.0 funcionó; Deferred Fog 2.0.0 quedó
inactivo; TP Classic UI 1.3.1 crasheó. c6 no añade funciones: hace que el gate, el navegador y la documentación digan esa verdad.

* **Una sola fuente de política.** `src/dusk/mods/hardware_findings.inc` lista, por **id + versión exactos (+ SHA-256 del `.dusk` cuando se
  conoce)**, solo hechos demostrados en consola: `Tested`, `Inactive`, `Incompatible`, `Unverified`, y las variantes `switch-arm64` que H38 entrega
  (`DUSK_MOD_SWITCH_VARIANT`). El juego la compila (`hardware_findings.hpp`, todo `constexpr`) y el gate la lee (`h38_policy.hardware_findings`);
  ninguno tiene otra lista. Lo que no está listado (otra versión, otros bytes, otro mod) es `unverified`: nunca lo bloquea una entrada escrita
  para otra cosa. No se copia la auditoría de 59 mods al NRO.
* **Gate (perfil h38).** Cada paquete del informe lleva `static_compatible` (lo comprobado estáticamente), `hardware_status`/`hardware_reason`
  (la consola) y `compatible` (ambos). Las causas estáticas siguen en `rejections`; la de hardware va aparte en `hardware_rejection`. Un paquete
  `inactive`/`incompatible` nunca es `compatible`, no cuenta en los perfiles de capacidad y, si se entrega como required/optional, bloquea READY.
  Uno de los cinco opcionales que solo está en `--catalog` se informa como `catalog` (no bloquea). Nuevo chequeo estático: **los hooks sobre
  miembros virtuales se resuelven como los resuelve el cargador** (nombre cualificado del overrider; si no, la vtable de la clase), contra el
  manifiesto. Sobre los artefactos exactos de c5 el gate corregido da NOT READY con los dos fallos de la consola: el hook de Deferred Fog y el
  hallazgo de TP Classic, y la capacidad sin TP Classic baja de 500 a 487 páginas.
* **Navegador (solo Switch).** Con el descriptor de descarga (id, versión y SHA-256 del catálogo): TP Classic 1.3.1 muestra **Incompatible with
  this build** con su causa y el botón no instala ni actualiza (se puede abrir y leer). Una versión futura de TP Classic se muestra **Unverified**,
  no bloqueada. Randomizer y Twilit muestran **Switch build included**: la copia del catálogo es la build Linux y nunca se instala ni sustituye a la
  `switch-arm64` (ni desde la ficha, ni desde la comprobación de actualizaciones, ni en la cola de descargas). La ficha añade una línea «Tested /
  Unverified / Incompatible on Switch» para los mods listados.
* **Deferred Fog: causa real.** El mensaje de c5 («class name is not representable as a vtable symbol») era solo la segunda razón. La primera,
  que el cargador descartaba, era que `dBgp_c::modelMaterial_c::drawSimple` **no existe en el ELF**: `dBgp_c` solo funciona en builds DEBUG
  (`dStage_roomControl_c::getBgp` devuelve NULL en Release; `drawShare` es solo SHIELD_DEBUG) y `--gc-sections` lo eliminó, con su vtable. Ni el
  nombre completo ni la vtable anidada (`_ZTVN6dBgp_c15modelMaterial_cE`) podían resolver; formar el símbolo anidado no lo habría arreglado.
  Arreglo genérico, sin reglas por mod: (1) `platforms/switch/mod_hook_roots.txt` conserva la función (mismo mecanismo que `drawOxygen` de
  Dawnlight); el hook carga y nunca se dispara, porque ninguna build Release ejecuta `dBgp_c`; la niebla la hacen los otros hooks del mod;
  (2) el cargador informa las dos razones; (3) el gate comprueba los hooks de miembros virtuales. No se toca el `.dusk`, el SDK ni el formato de
  metadatos. El SDK sigue dejando vacío el símbolo de vtable de una clase anidada (`vtable_symbol` en `sdk/include/mods/meta.hpp`); el cargador
  ya acepta un `_ZTVN…E` correcto (probado), pero cambiar el SDK no ayuda a paquetes ya publicados y queda fuera de esta beta.
  **Que pase en el host no lo marca `tested`:** queda `unverified` hasta que la consola muestre que cambia la niebla.
* **TP Classic UI 1.3.1: `incompatible`.** Su biblioteca linux-aarch64 es Debug (`DEBUG=1`, `.debug_info`): lee `dComIfG_play_c` con el layout
  Debug (+32 bytes antes de `mWindow`). No se cambia el layout del juego, no hay shim de offsets ni parche de bytes. Investigación acotada (P2):
  fuente pública MIT `https://github.com/XandasL/TP-CM-Controller-UI`, compilada por GitHub Actions con `CMAKE_BUILD_TYPE=RelWithDebInfo` contra
  Dusklight fijado en `d281c34a65568ee7be2319985f3a290c0ef65abc`; el repositorio **no tiene tags**, así que no se puede fijar el commit exacto de
  1.3.1. Una reconstrucción `switch-arm64` exigiría: fijar ese commit (con el autor o por fecha y comprobación de bytes de la build Linux),
  compilarlo en Release contra el SDK de H38 (no `d281c34a`, cuyo layout no está comparado) y probarlo en consola. No se hace en c6.
* **Capacidad.** 512 páginas no cambian. Sin TP Classic, el conjunto principal + los cuatro opcionales restantes = 487 páginas; no se recomienda
  probar los opcionales juntos: 12–25 páginas libres ocultan fallos individuales.

### 3.6 H38-c7: Deferred Fog 2.0.0 incompatible

En la consola, con c6 (`SWITCH-TEST-H38-C6\logs-consola\RESULTADO-HARDWARE-H38-C6-2026-10-05.md`), Deferred Fog 2.0.0 se activó (sus 11 hooks
se instalan: el arreglo de `drawSimple` funcionó) y **crasheó** al compilar su `res/fog.wgsl`: el hilo trabajador de Dawn/Tint agota su pila
dentro de `tint::resolver::Resolve` / `AnalyzeUniformity` (Data Abort, SP por debajo de la pila del hilo). c7 solo cambia su entrada en
`hardware_findings.inc` (id, versión 2.0.0 y SHA-256 `da70c3c2…8b` exactos) a `Incompatible`: el navegador, las actualizaciones y la cola lo
bloquean como a TP Classic; otra versión sigue `Unverified`. El gate conserva sus resultados estáticos, lo marca `compatible: false` y no lo
cuenta en la capacidad; en la entrega c7 vive solo en `catalog-audit\` (no bloquea READY). Capacidad sin TP Classic ni Deferred Fog: **476 de
512 páginas**; con TP Classic 1.3.2 (13 páginas, gate sobre el ELF de c7): **489 de 512**, 23 libres. No se arregla la pila de Dawn/Tint en la beta (candidato para V1).

**Matriz pública tras c7** (la de 3.4 queda como histórico):

| Categoría | Mods |
|---|---|
| **Bundled / tested on Switch** | Randomizer 1.0.6 y Twilit Essentials 2.5.0 (los paquetes `switch-arm64` entregados con H38, **no** las copias Linux del catálogo), Cosmetics, Roll and Sprint, runtime Luau |
| **Tested compatible** | Cowboy Link 1.1.0; TP Classic Modern Controller UI **1.3.2** (paquete Release `8ea68862…452e`, probado el 2026-10-05 con Loader Mode Execute, biblioteca linux-aarch64 sin `.debug_info`; desactivarlo en plena partida terminó una sesión con `FATAL convert_texture: unknown texture format 144`) |
| **Statically compatible, hardware unverified** | Fairy Companion 1.0.0, KH2 Camera 1.0.1; y los demás que `H38-CATALOG-COMPATIBILITY-AUDIT.md` da por *static pass* |
| **Known incompatible** | el paquete Debug de TP Classic Modern Controller UI 1.3.1 (`be891f39…22fd`) y Deferred Fog 2.0.0 (crashean: no instalar); Dawnlight 3.8.1 y ALBW 0.3.0 del catálogo (estático) |
| **Not audited** | cualquier otro mod o versión |

## 4. Pipeline general de mods `switch-arm64`

`platforms/switch/mods/h38-sources.json` lista cada mod con URL, commit **y árbol git** fijados, su directorio de
parches y cómo construir el paquete; añadir un mod es añadir una entrada. `build_switch_mods.py` (envuelto por
`build-h38-mods.sh`), por mod: resuelve la fuente (`--source`, el submódulo o un `git fetch` del commit), exige que
el árbol sea el fijado, exporta un árbol limpio, aplica los parches en orden, configura con
`SwitchArm64Mod.toolchain.cmake` y el SDK de este checkout, reempaqueta de forma canónica (`make_dusk.py`, con
exactamente `lib/switch-arm64/mod.so`), ejecuta `check_switch_module.py` y `package_audit.describe` y escribe un
informe sin reloj ni rutas absolutas (`h38-mods-report.json`, `SHA256SUMS`).

Dos fases: `packages` y luego `exports`, que escribe `OUT/mod_required_symbols.txt` y `.diff` **sin tocar** la lista
confirmada. Con `--phase exports` el NRO aún no existe: las exportaciones se arreglan antes de la única compilación.

No hay TLS dinámico: un paquete Linux con TLS/TLSDESC o con la ABI de streams insegura se rechaza, sin excepciones
por id. `build-h37-test-mods.sh` y sus invocaciones H37 no cambian.

### Twilit Essentials 2.5.0

Fuente fijada: `https://github.com/F1mmel/dusklight-twilit-essentials` @ `7682060c20e2e3757b2888a27bbd958506ccb4f7`
(`mod.json` 2.5.0; solo coincide la versión declarada, no se afirma que el artefacto de catálogo salga bit a bit de
ese commit). No es submódulo; sus parches están en `platforms/switch/patches/twilit_essentials/`:

1. `0001` — `collection_lib` se compila con `STBI_NO_THREAD_LOCALS` en la toolchain switch-arm64 (su único TLS es el
   motivo de fallo de stb_image). Es seguro porque stb_image tiene un único usuario, `tex_replacements_apply()`
   (decodificar, `stbi_failure_reason`, liberar), que corre en el hilo del juego y ni la librería ni el mod crean
   hilos; el código lo afirma: el primer llamador es el dueño y otro hilo recibe el fallback sin decodificar.
2. `0002` — CMake hermético: sin reescribir el README, sin regenerar el logo y sin copiar a la carpeta de mods del
   usuario.

`src/d/actor/d_a_e_rdb.cpp#damage_check` es `DUSK_NOINLINE` desde H38-c1: H37-c8 demostró (`h37-candidate.json`) que
`-O3` lo integraba y era el único hook ausente de Twilit y ALBW. El cuerpo no cambia, y el gate sigue informando con
precisión si algún ELF futuro lo da por ausente (`h38_policy.KNOWN_HOOK_NOTES`).

`FEATURES webgpu` pide las cabeceras de Dawn; el SDK las descarga como paquete. Sin red:
`DUSK_WEBGPU_INCLUDE_DIR=<dir con webgpu/webgpu.h de la revisión de Dawn de Aurora>` (usa
`SwitchModWebGPUHeaders.cmake`).

Pruebas: `test-switch-mod-pipeline.sh` (manifiesto y commit fijados, árbol incorrecto, parche que no aplica,
paquete canónico, aplicación determinista desde dos directorios, selección de `switch-arm64`, diff de exports
separado, Twilit obligatorio solo con la opción, rechazo de un paquete Linux con TLS, capacidad combinada) con
`cmake` y `symgen` falsos y un ELF AArch64 sintético; `test-twilit-patches.sh` con la fuente real fijada
(`TWILIT_SRC=<checkout>` o descarga del commit).

### A Link Between Twilight (ALBW) 0.3.0

Fuente fijada: `https://github.com/WadeWinningWilson/albt-dusks` (el port modular, **no** el fork completo
`A-Link-Between-Twilight`) @ tag `v0.3.0`, commit `5cf4a362b128834199baaaecf8ab56aca9130769`, árbol
`77fb2d415d59df9cf19da61ffa5b7508393907fc`, `mod.json` `dev.albt.albw` 0.3.0. Sin parches (el directorio
`platforms/switch/patches/albw` no existe a propósito). Su CMake trae su propio `FetchDusklight.cmake`; el pipeline pasa
`DUSKLIGHT_DIR` = este checkout y se niega a empezar si ese checkout lleva un `.stamp` o no tiene `sdk/CMakeLists.txt`
(descargaría otro SDK en silencio). La fuente no tiene `thread_local`; su único `std::ifstream` impide aceptar el
binario Linux, pero no reconstruirlo con devkitA64 como `switch-arm64`. El binario Linux del catálogo no se acepta.

ALBW es un paquete **opcional**: un `albw.dusk` `switch-arm64` en el directorio de paquetes del gate se juzga como
cualquier módulo `switch-arm64` (sin TLS, pins de id y versión) y su ausencia nunca bloquea. No se habilita junto con
Twilit (ver capacidad).

**Resultado de H38-c1: ALBW no se entrega.** El pipeline lo compila desde la fuente fijada, pero 2 de sus 117 unidades
no compilan para `switch-arm64`: `src/albw_l1_input.cpp` y `src/soul_of_light.cpp` incluyen `<dlfcn.h>` y resuelven por
nombre, en tiempo de ejecución, símbolos del juego y de SDL (`dlsym(RTLD_DEFAULT, …)`: `PADGetIndexForPort`,
`PADGetSDLGamepadForIndex`, `PADGetKeyButtonBindings`, `SDL_GetGamepadButton` y `fopAcM_fastCreate` con su nombre
mangled). Horizon no tiene `dlfcn.h`, y un `dlsym` real exigiría que el cargador sirva búsquedas de símbolos por cadena
(el manifiesto de símbolos solo guarda lo que los paquetes piden como import o hook): una función nueva del cargador
o reescribir esas funciones de ALBW, no un arreglo pequeño. Por eso no se parchea ni se simula (un `dlsym` que siempre
falla desactivaría en silencio la entrada L1 y la caída de la orbe). Medición con un `dlfcn.h` que no encuentra nada,
solo como diagnóstico y nunca empaquetado: el resto de ALBW compila, no tiene problemas de módulo ni bloqueos del
cargador, declara 331 páginas de hooks (6 objetos), 66 hooks por nombre y 10 servicios; el único bloqueo real es
`dlfcn`. El binario Linux del catálogo sigue rechazado. Si ALBW se retoma: o bien un servicio `dlsym` en el cargador
(con sus símbolos en el manifiesto), o bien un parche de ALBW que enlace esas cinco funciones directamente.

### Capacidad por perfiles

`h38_policy.capacity_profiles` calcula, con las páginas medidas de los paquetes `switch-arm64` reales, contra
`CODE_ARENA_MAX` = 8 objetos de 64 páginas (512 páginas): `primary` = base + Randomizer + Twilit (obligatorio);
`albw` = base + ALBW (un ALBW que no cabe ni solo no se entrega); `albw_randomizer` = base + Randomizer + ALBW
(alternativa, solo tras probar ALBW aislado en consola); `twilit_albw` (nunca admitido). Que quepa en la tabla no dice
que el pool de CodeMemory del sistema vaya a darlos: es riesgo de hardware.

### Preferencia por la copia Switch (cargador)

`src/dusk/mods/loader/package_choice.*` (solo `std`): en la compilación Switch, para un mismo id gana la versión más alta
**entre las copias con `lib/switch-arm64/mod.so`**; solo sin copia Switch se aplica la versión más alta de siempre
(y el fallback Linux). Los paquetes sin código nativo no se degradan y los empates (directorio, ruta) no cambian. Una
instalación en caliente clasifica el paquete nuevo igual que el escaneo; una actualización con código nativo pero sin
`switch-arm64` mientras existe una copia Switch se rechaza antes del reemplazo atómico («This update has no
switch-arm64 library; the compatible Switch version was kept.»). Esto decide qué paquete se prueba; no relaja
`elf_runtime_policy`, `elf_audit.py` ni los bloqueos TLS/ABI. Prueba: `test-switch-package-preference.sh`.

## 5. Gate H38

`check_h38_candidate.py` = `check_h37_candidate.py --profile h38` (+ `h38_policy.py`). Exige: los cuatro paquetes
oficiales de H37; Randomizer `switch-arm64` con `PT_GNU_EH_FRAME`; Twilit `switch-arm64` 2.5.0 cuando
`ALEKS2_H38_REQUIRE_TWILIT=ON` (en el gate, `--require-twilit`; en CMake, la opción homónima, **OFF por defecto** y
leída solo por `dusklight_h38_candidate`); imports, hooks, servicios y política ABI compatibles contra el ELF final;
ningún TLS/TLSDESC en un módulo `switch-arm64` (H38 no lo soporta); y las páginas de hooks de los paquetes cargados
juntos dentro de la tabla de arenas del cargador (`CODE_ARENA_MAX` = 8 objetos de 64 páginas; no se cambia sin un
fallo medido). `ready=false` mientras falte el ELF final. Desde c5, además, cinco paquetes oficiales opcionales (3.4): si se
entregan en `--packages` deben pasar y cuentan en la capacidad; si faltan no cambian nada. Desde c6 (3.5), el informe separa
`static_compatible` de `hardware_status` (`src/dusk/mods/hardware_findings.inc`), un paquete `inactive`/`incompatible` en consola no es
`compatible` ni cuenta en la capacidad, y los hooks sobre miembros virtuales se comprueban contra el manifiesto. **READY certifica la candidata y
sus requisitos obligatorios; no certifica el comportamiento de los mods del catálogo.**

## 6. Secuencia local obligatoria (PC con devkitA64)

En Windows, como en H37, el repositorio y `OUT_DIR` no pueden tener espacios (`subst X: "<ruta>"` y trabajar en `/x`).

```bash
# 1. fast-forward al hash final de la nube
git fetch origin claude/h38-development && git merge --ff-only origin/claude/h38-development
git submodule update --init extern/aurora mods/randomizer      # Aurora cf9b2d1 (rama claude/h38-development de aurora-aleks-nx)

# 2. Randomizer y Twilit con devkitA64, sin NRO (SYMGEN = el symgen que fija cmake/SymbolManifest.cmake)
export SYMGEN=<symgen>  TWILIT_SRC=<checkout con 7682060>     # RANDOMIZER_SRC y *_SRC como en H37; DUSK_WEBGPU_INCLUDE_DIR sin red
platforms/switch/tools/build-h38-mods.sh packages /x/h38-out randomizer twilit_essentials

# 3. auditoría: el propio comando imprime check_switch_module / package_audit y escribe /x/h38-out/h38-mods-report.json
#    (exit 1 si algún paquete tiene problemas)

# 4. diff de exports (no modifica nada del repositorio)
platforms/switch/tools/build-h38-mods.sh exports /x/h38-out randomizer twilit_essentials

# 5. solo si hay diff: aplicar y commitear ÚNICAMENTE esos exports (y un DUSK_NOINLINE si el ELF lo demuestra)
cp /x/h38-out/mod_required_symbols.txt platforms/switch/mod_required_symbols.txt
git add platforms/switch/mod_required_symbols.txt && git commit -m "H38: exports for the switch-arm64 Randomizer and Twilit"

# 6. directorio de paquetes del gate: los cuatro oficiales de H37 + los dos de H38
mkdir -p /x/h38-pkgs && cp <H37 mods>/{loader_lab,cosmetics,rollandsprint}.dusk /x/h38-out/{randomizer,twilit_essentials}.dusk /x/h38-pkgs/
cmake --preset switch-devkitA64-release-h38 -DALEKS2_H38_PACKAGES_DIR=/x/h38-pkgs    # build limpio; Twilit requerido

# 7. UNA sola compilación del NRO (H38-c1)
cmake --build --preset switch-devkitA64-release-h38

# 8. gate final y verify-nro.py
cmake --build build/switch-devkitA64-release-h38 --target dusklight_h38_candidate     # h38-candidate.json: ready=true
python verify-nro.py ...                                                              # la herramienta local de siempre

# 9. solo después, preparar la prueba de hardware
```

Si el paso 4 no produce diff, el 5 se omite y el NRO se compila con la lista que ya está. Si el gate del paso 8 pide
`DUSK_NOINLINE` para `damage_check`, ese es un segundo commit y otra compilación: por eso el paso 2 en adelante se
hace antes de tocar el NRO.

## Medido en el PC (H38-c1) y qué solo se ve en la consola

Con devkitA64 reales y desde cero (dos veces, mismos bytes): Randomizer `switch-arm64` con los parches 0001–0009
(92 páginas de hooks, 2 objetos) y Twilit Essentials 2.5.0 `switch-arm64` (343 páginas, 6 objetos; el paquete Linux
estimaba 269/5). Base + Randomizer + Twilit = 467 páginas = 8 objetos de CodeMemory: cabe en la tabla pero sin margen,
y el pool del sistema es compartido, así que es riesgo de consola, no un hecho. El diff de exports solo toca las líneas
de Twilit (su build Switch pide nombres de newlib/libstdc++ 16, más 8 hooks por nombre de 2.5.0).

**H38-c2.** El gate de H38-c1 dio `NOT READY` por una sola causa que solo se ve con el ELF final: Twilit 2.5.0 `switch-arm64`
importa `fmod` y `fmodf`, que el NRO enlaza pero deja locales (ocultos) como `sqrt`/`sqrtf`, y no estaban en la tabla
`aleks2_image_runtime` (`elf_image_runtime.c`). H38-c2 los registra igual que `sqrt`/`sqrtf`, en las propias funciones de la
imagen, y nada más (los paquetes, Randomizer, Twilit, Aurora y los exports no cambian). Pruebas:
`test-image-runtime-table.sh` y `test-symbol-manifest.sh` (resuelve ambos por la tabla leída de un ELF enlazado).

Sin consola: `swkbdShow`, el comportamiento de los gestos con Joy-Con reales (ejes del Nunchuk, umbrales), la descarga
y recarga de Randomizer sin `stale handle`, Twilit en Dry Run y Execute, y las arenas con Randomizer + Twilit.
