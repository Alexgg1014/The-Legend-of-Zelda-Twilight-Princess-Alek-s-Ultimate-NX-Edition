# Ruta de trabajo para Dusklight Switch 1.0

Documento operativo para continuar el port sin depender de pruebas manuales ni de un NRO distinto por cada ajuste.

> **Estado vigente (2026-10-01): la base validada en hardware es H32-c1-b84a849829; H31-c1 es el fallback de rendimiento.**
> Este documento se escribió en el corte H29 y se amplió después de H30. Las secciones hasta «Referencias» conservan ese corte como evidencia histórica: donde dicen «H29-c1» como base, vale lo indicado aquí y en «Actualización 2026-10-01: estado tras H32», al final, que también clasifica cada bloque en completado, validado, pendiente, propuesta e investigación futura.
> Los documentos de contexto `HITO6.md`, `HITO7_MODS_NATIVOS.md`, `PROVENANCE.md` y `PLAN_DEKO3D.md` que se citan abajo pertenecen al directorio de trabajo original y **no están en este repositorio**; la procedencia vigente está en [switch-h32-provenance.md](switch-h32-provenance.md). Tampoco está `tools/make-shader-seed.py` (mencionado en el Entregable B).

## Punto de partida

- Fecha del corte: 2026-10-01.
- Rama: `aleks2`.
- Base funcional validada: H29-c1, commit `04e1eaa087` (`H29-c1-04e1eaa087`).
- Última validación física disponible: H29-c1, probada el 2026-10-01 sin crashes, errores de log ni defectos visuales observados.
- Objetivo de rendimiento para 1.0: 30 FPS estables sin overclock.
- Roll and Sprint funciona en Switch, pero actualmente se compila dentro del NRO mediante `StaticMods.cmake`.
- Cross-save en la nube pasa a ser objetivo de 1.0, distribuido como mod nativo multiplataforma; en Switch el mod se compila estáticamente dentro del NRO.
- Los assets mods se cargan desde la SD.
- La superficie ABI conserva los símbolos requeridos por los 22 mods publicados que se auditaron, pero aún no existe un cargador dinámico ELF que abra sus `.so` desde la SD.

Este documento no reemplaza `HITO7_MODS_NATIVOS.md` (fuera de este repositorio). HITO7 conserva la investigación histórica del ABI; este archivo organiza el trabajo restante y las pruebas de aceptación.

### Corte H28/H29

- H28-c1 integró Dusklight v2.0.3 y añadió el benchmark reproducible de 60 segundos, reporte JSON, bloqueo temporal de guardado, cancelación y restauración del contexto.
- H28 dejó una línea base de dos ejecuciones calientes en `F_SP122`, sala 8, Flip Grip 733x540 y clocks stock de portátil: 23,05 FPS medios, Execute 41,5 ms y DrawRec 22,8 ms.
- H29-c1 retiró el instancing histórico, reutilizó el `PipelineConfig` ya derivado y amplió el reporte a esquema 2 con contadores de rebuilds y fusiones bloqueadas.
- Una ejecución caliente de H29-c1 en las mismas condiciones observó 26,49 FPS, Execute 36,1 ms y DrawRec 16,7 ms. Es una mejora observada de aproximadamente 15 %, suficientemente grande para descartar una regresión clara, pero no una cifra definitiva porque solo se ejecutó una vez.
- Los primeros contadores de H29 registraron aproximadamente 986 `pipeline_rebuilds` y 1360 `uniform_rebuilds` por frame. Son invalidaciones/reconstrucciones de estado, no compilaciones de pipelines, y quedan como pista para análisis estático antes del siguiente NRO.
- Artefacto validado: `SWITCH-TEST-H29/dusklight-h29-c1.nro`, SHA-256 `2de0f2b8dc644547f469f7361ca42c00562697443db04eccb9d01c4744690e06`.

El detalle del flujo, formato del reporte y validación está en [switch-h28-diagnostics.md](switch-h28-diagnostics.md).

## Decisiones para 1.0

1. Usar 1600x900 como resolución automática en dock.
2. Mantener 1280x720 en portátil y 960x540 en Flip Grip.
3. Dejar 1080p como opción manual de calidad, no como valor automático.
4. No habilitar DRS por defecto hasta superar una prueba de estrés sin crecimiento de memoria ni device loss.
5. Crear primero un NRO de laboratorio con benchmark, telemetría y opciones en tiempo de ejecución.
6. Separar la preparación de sombreadores del benchmark de rendimiento.
7. Distribuir una lista compacta de pipelines; compilar la caché específica del driver en la propia consola.
8. Probar el cargador `.so` por niveles, pero cambiar los mods de prueba desde la SD sin recompilar el NRO.
9. Considerar ALBW una prueba integral de compatibilidad, no el primer mod de bring-up.
10. Optimizar el navegador para dos tarjetas por fila y evitar que la decodificación/subida de imágenes produzca tirones.
11. Añadir un perfil opcional de controles estilo Wii; el esquema tradicional debe seguir disponible y completo.
12. No publicar mientras quede código atribuible a souldbminer sin reemplazar, por respeto a su petición.
13. No desarrollar un backend Deko3D en esta ruta. Conservar la investigación solo para evitar reabrir la decisión sin evidencia nueva.
14. Incluir cross-save en la nube en 1.0 con sincronización de snapshots completos `.gci`, conflictos explícitos, respaldo local y transporte HTTPS. No sobrescribir una partida divergente automáticamente.
15. Mantener un único mod funcional en Windows, Linux, macOS, Android y Switch; construir el artefacto ARM64 de Android y compilar el mismo código como mod estático de Switch.
16. Empezar con Firebase Spark sin enlazar facturación; al alcanzar una cuota, pausar la sincronización y conservar el guardado local, sin aceptar cargos automáticos.

## Regla principal para reducir builds

Un build debe responder varias preguntas relacionadas. No crear un NRO para cambiar un único valor que pueda ser una opción de ejecución.

El NRO de laboratorio deberá permitir modificar, sin recompilar:

- Resolución interna y perfil gráfico.
- Sombras, agua, bloom, DoF y cadencia del acompañante.
- DRS y sus escalones.
- Inicio del benchmark corto.
- Inicio de la matriz automática.
- Inicio del estrés de DRS.
- Inicio de la preparación de sombreadores.
- Activación del cargador dinámico experimental.
- Selección de un único mod de prueba o carga secuencial de una batería de mods.
- Nivel de log del cargador.

Los cambios de driver, flags de compilación, afinidad de threads, LTO/PGO o arquitectura del cargador sí pueden requerir otro NRO. Los archivos `.dusk`, sus versiones y la combinación de mods no deben requerirlo.

## Entregable A: benchmark reproducible

### Benchmark rápido

Duración objetivo: 60 segundos de medición, más calentamiento.

Flujo:

1. Entrar desde una opción de Diagnóstico en el menú principal.
2. Guardar temporalmente ajustes y contexto que deban restaurarse.
3. Cargar una escena pesada y conocida, inicialmente `F_SP122`.
4. Esperar a que termine la carga y dejar 5-10 segundos de asentamiento.
5. Esperar `queuedPipelines == 0`, con timeout visible.
6. Ejecutar una cámara fija o una ruta determinista durante 60 segundos.
7. Bloquear el input que pueda alterar la prueba.
8. Escribir el resultado en JSON y mostrar un resumen.
9. Restaurar los ajustes. No escribir sobre el save normal.

Datos mínimos del reporte:

- Commit y etiqueta de build.
- Driver, adaptador y esquema de caché.
- Dock, portátil o Flip Grip.
- Resolución y perfil gráfico.
- FPS promedio y 1% low.
- Frametime mediano, p95 y p99.
- Porcentaje de frames mayores de 33.3 ms y de 50 ms.
- Promedios y máximos de las zonas CPU existentes: Sim, Actors, Draw, Present, Audio y UI.
- Tiempo GPU si `TimestampQuery` está disponible.
- Draws enviados, draws combinados y pipelines creados/pendientes.
- Número de cambios de DRS.
- Clocks detectados, sin modificarlos.
- Mod y acompañante activos durante la prueba.

No usar solo FPS promedio: puede ocultar stutter grave.

### Matriz automática

Una ejecución larga puede probar configuraciones serialmente dentro del mismo NRO:

- 900p, 720p y 540p.
- Sombras 1/2/4.
- Agua activada/desactivada.
- DoF activado/desactivado.
- Bloom moderno/clásico.
- Acompañante activado/desactivado y distintas cadencias.
- DRS activado/desactivado.

Cada combinación debe tener un calentamiento corto y un tramo de medición. La salida será una tabla JSON/CSV ordenable por p99, porcentaje de frames perdidos y promedio.

### Aceptación

- Tres repeticiones de una misma configuración no deben variar más de 3-5% salvo una carga externa registrada.
- La prueba debe ejecutarse sin tocar el save del usuario.
- El reporte debe distinguir caché fría, caché caliente y compilación en curso.
- Dock, portátil y Flip Grip se validan por separado.

Piezas existentes: `src/dusk/profiler.hpp`, `src/dusk/commands.cpp` y `src/dusk/session_log.cpp`.

## Entregable B: preparación de sombreadores

### Dos cachés diferentes

- `pipeline_cache.db`: configuraciones portables de pipelines. Es una lista de recetas y debería ser relativamente pequeña.
- `dawn_cache.db`: blobs compilados por Dawn/NVK para un driver concreto. Puede crecer varios MB y es menos portable.

Aurora ya puede leer `initial_pipeline_cache.db` e `initial_dawn_cache.db` desde RomFS. El script `tools/make-shader-seed.py` elimina bookkeeping, ejecuta `VACUUM` y conserva la identidad del driver. Las entradas de Dawn se comprimen con Zstd cuando `AURORA_CACHE_USE_ZSTD` está habilitado.

No asumir el tamaño de la caché. Generar primero una caché maestra representativa y registrar:

- Cantidad de filas.
- Suma de tamaños sin comprimir.
- Tamaño final de SQLite.
- Aumento real del NRO.

### Shader Tour de desarrollo

El tour sirve para descubrir configuraciones de pipelines y construir la lista maestra. No es un benchmark de FPS.

Duración objetivo: 10-20 minutos.

Cada escenario debe:

1. Hacer warp.
2. Esperar carga completa.
3. Esperar actores y efectos.
4. Recorrer varias cámaras o girar 360 grados.
5. Activar variantes relevantes.
6. Esperar a que la cola de pipelines nuevos se estabilice.
7. Continuar al siguiente escenario.

Cobertura inicial:

- Ordon, Hyrule Field e interiores.
- Bosque/vegetación.
- Agua y reflejos.
- Niebla, nieve, lava y Twilight Realm.
- Mazmorras representativas.
- Día, noche y clima cuando cambien materiales/efectos.
- Link humano, lobo, Midna y acompañante.
- Enemigos comunes, miniboss y bosses representativos.
- Cutscenes.
- Inventario, mapa, diálogos y HUD.
- Sombras, agua, bloom y DoF en las configuraciones que se distribuyan.

Un warp inmediato no basta: muchos pipelines solo se crean cuando un actor, material o efecto llega a dibujarse.

### Experiencia del usuario

La consola del usuario no debe recorrer mapas. Recibe la lista compacta de pipelines y la compila localmente:

> Preparando sombreadores
>
> Dusklight está optimizando los gráficos para esta consola. Esto solo debería ser necesario una vez o después de una actualización gráfica importante.

La pantalla debe mostrar:

- Barra y contador, por ejemplo `312 / 584`.
- Tiempo aproximado solo cuando haya suficientes muestras para estimarlo.
- Botón Omitir/Continuar; jugar antes de terminar puede causar tirones temporales.
- Reanudación: lo ya guardado en `dawn_cache.db` no se pierde si se interrumpe.
- Motivo de recompilación: primer inicio, driver diferente o versión incompatible del pipeline/esquema.

Clave del marcador de finalización:

- Identidad exacta del driver.
- Versión del esquema de pipeline y caché.
- Firma de la lista inicial distribuida.
- Firma de las opciones que cambien el layout del render target.

### Política recomendada para 1.0

- Incluir `initial_pipeline_cache.db`.
- No incluir inicialmente una `initial_dawn_cache.db` completa.
- Evaluar una Dawn seed recortada solo para arranque, menú y primera zona si su tamaño es bajo.
- No descargar cachés desde Internet en 1.0.
- Si se agrega descarga posteriormente: manifest firmado o SHA-256, driver exacto, archivo temporal, instalación atómica y aplicación en el siguiente arranque.

### Aceptación

- Una SD limpia muestra la preparación y termina sin bloquear la UI.
- Una segunda ejecución no repite trabajo costoso.
- Interrumpir y reanudar no corrompe SQLite.
- La cola llega a cero y los pipelines encontrados por el tour permanecen disponibles en una ejecución posterior.
- La precarga no se cuenta dentro de un benchmark de rendimiento.

## Entregable C: resolución y DRS

### Valores automáticos

- Dock: 1600x900.
- Portátil: 1280x720.
- Flip Grip: 960x540.
- 1920x1080: opción Calidad/Personalizada.

900p reduce aproximadamente 30.6% los píxeles frente a 1080p y mantiene bastante más detalle que 720p.

### DRS

El DRS actual cambia los render targets internos, pero existe un antecedente de crash tras varios cambios y, en escenas limitadas por CPU/render worker, bajar resolución puede no producir una ganancia relevante.

Antes de habilitarlo:

1. Instrumentar creación/destrucción de texturas y memoria disponible.
2. Crear un modo de estrés que repita `900 -> 720 -> 540 -> 720 -> 900`.
3. Ejecutar al menos 100 transiciones y 30 minutos.
4. Registrar cada transición, pausa GPU, memoria y device loss.
5. Probar liberar explícitamente después de GPU idle.
6. Si la liberación diferida sigue creciendo, evaluar reutilizar un conjunto pequeño de render targets por escalón.

Política inicial:

- Objetivo de 30 FPS.
- Pocos escalones.
- Histéresis de 5-10 segundos.
- No cambiar durante cargas o mientras se compilan pipelines.
- Bajar un escalón por decisión y medir si realmente mejoró el frametime.
- Revertir si el cuello era CPU y no se obtiene una mejora significativa.

El DRS solo entra habilitado por defecto si la prueba de estrés no muestra crecimiento sostenido, crash ni device loss.

## Entregable D: navegador de mods

### Estado actual

`ModBrowser` ya declara un `NavGroup` tipo grid con dos columnas y las tarjetas usan aproximadamente 48% del ancho. Sin embargo:

- La barra de filtros ocupa 232-264 dp.
- Cada tarjeta exige 220-260 dp como mínimo.
- El padding y el gap hacen que en el ancho lógico de Switch no quepan dos y la segunda se envuelva.

El proveedor de imágenes además:

- Solicita banners de hasta 640 px e iconos de 128 px para la lista.
- Permite cuatro solicitudes simultáneas.
- Decodifica PNG al completar una solicitud.
- Mantiene hasta 64 MB de imágenes decodificadas.
- Libera/recrea texturas cuando llega la imagen real.

Esto puede causar tirones cuando varias imágenes terminan y se suben cerca del mismo frame.

### Cambios de layout

- Forzar dos tarjetas por fila con `min-width: 0` y un basis calculado.
- En pantallas estrechas, convertir la barra lateral en botón/panel de Filtros o una fila compacta.
- Usar tarjetas más bajas en 720p/540p.
- Limitar el resumen a dos o tres líneas.
- Mantener navegación direccional correcta entre dos columnas.
- Usar skeletons para no reemplazar toda la pantalla por un estado vacío durante refresh.

### Cambios de rendimiento

- Pedir banners de 320-384 px para la lista.
- Pedir iconos de 64-96 px.
- Cargar solo tarjetas visibles y una fila adelantada.
- Decodificar PNG fuera del hilo principal.
- Subir como máximo una textura por frame o aplicar un presupuesto de 1-2 ms.
- Priorizar banner visible; diferir iconos secundarios.
- Agregar caché comprimida en SD con límite y llave por URL/hash/ETag.
- Conservar el catálogo anterior mientras se actualiza.
- Actualizar el estado de una tarjeta sin reconstruir todo el documento cuando sea posible.

### Medición

Agregar zonas de profiler para:

- Fetch/parse del catálogo.
- Construcción del DOM.
- Decodificación PNG.
- Subida de textura.
- Tiempo de update del navegador.

La aceptación es scroll y navegación sin frames mayores de 50 ms causados por imágenes en una página normal.

Archivos principales: `src/dusk/ui/mod_browser.cpp`, `res/rml/mod_browser.rcss` y `src/dusk/ui/remote_texture_provider.cpp`.

## Entregable E: cargador dinámico de mods `.so`

### Qué funciona hoy

- Asset mods desde SD.
- Servicios y API de code mods en Switch.
- Hooks AArch64/funchook dentro del NRO.
- Roll and Sprint compilado estáticamente.
- Shims de glibc/newlib y aliases requeridos por los paquetes auditados.
- Los símbolos requeridos por 22 mods publicados están preservados/exportados en la superficie de Switch.

### Qué no significa lo anterior

Resolver símbolos no carga un módulo. `switch_native_module.cpp` sigue usando una tabla de módulos integrados. Un ID no compilado dentro del NRO se rechaza.

### Trabajo del cargador ELF64 AArch64

1. Validar ELF, arquitectura AArch64, endianess, clase y límites de todos los offsets.
2. Leer `PT_LOAD` y reservar memoria alineada para segmentos RX/R/RW.
3. Copiar datos, poner a cero BSS y rechazar rangos solapados o fuera del archivo.
4. Leer `PT_DYNAMIC`, tablas de símbolos/cadenas y dependencias.
5. Resolver `DT_NEEDED` solo contra bibliotecas permitidas o runtime libraries extraídas del paquete.
6. Aplicar las relocaciones presentes en los paquetes auditados. Como mínimo, revisar `R_AARCH64_RELATIVE`, `GLOB_DAT`, `JUMP_SLOT` y `ABS64`; no adivinar: generar una tabla desde los `.so` reales.
7. Soportar GNU hash/hash y, si aparecen, versiones de símbolos.
8. Implementar TLS si algún paquete incluye `PT_TLS` o relocaciones TLS.
9. Registrar información de unwind (`.eh_frame`) cuando sea necesaria para excepciones C++.
10. Ejecutar `.preinit_array`/`.init_array` según corresponda.
11. Resolver `mod_meta`, `mod_ctx`, `mod_initialize`, `mod_update` y `mod_shutdown`.
12. Aplicar protecciones finales sin dejar memoria W+X.
13. En descarga: llamar `mod_shutdown`, retirar hooks, ejecutar finalizadores y liberar solo cuando sea seguro.
14. Implementar safe mode y marcador de arranque para desactivar el último mod si el proceso murió durante su carga/inicialización.

### Diagnóstico obligatorio

Cada intento debe escribir una línea por etapa:

- Paquete y SHA-256.
- Ruta de la librería elegida (`linux-aarch64`).
- ELF validado.
- Segmentos reservados/copiados.
- Dependencias y símbolos resueltos.
- Número y tipos de relocaciones.
- TLS/unwind registrados o no requeridos.
- Constructores completados.
- Metadata y entrypoints encontrados.
- `mod_initialize` iniciado/terminado.
- Hooks instalados.
- Motivo exacto de rechazo.

Antes de ejecutar código debe existir un modo Dry Run que haga validación, dependencias, imports y relocaciones sobre memoria no ejecutable y produzca un reporte. Eso detecta muchos errores sin convertirlos en crashes.

### Escalera de compatibilidad

No probar veinte mods sin orden:

| Nivel | Mod | Qué valida | Criterio de salida |
|---|---|---|---|
| 0 | Módulo mínimo de laboratorio | ELF, relocaciones, init/shutdown | Carga y descarga repetida sin fuga/crash |
| 1 | Roll and Sprint dinámico | Game ABI y hooks ya conocidos | Misma conducta que la versión integrada |
| 2 | Deferred Fog o VBAO | Gfx/WebGPU y hooks gráficos | Activa/desactiva y cambia de escena |
| 3 | A Link Between Twilight | Actor, camera, config, game, gfx, hook, host, log, resource y UI | Arranque, combate, HUD, ajustes, cambio de zona y save temporal |
| 4 | Dawnlight | Gran superficie, combate, movimiento, bosses y recursos | Sesión prolongada y múltiples features |
| 5 | Twilit Essentials | HUD, modelos, menús, stamina, boss rush y configuración | Features representativas y reload |
| 6 | Randomizer oficial | Custom game mode, progresión, warps y saves | Crear/cargar partida y cambiar zona |
| 7 | Crests of Courage | HTTP, net y WebSocket | Se deja para el hito de red |

Datos del corte actual:

- `dev.bezide.dawnlight`: 982 símbolos preservados.
- `dev.albt.albw`: 962.
- `dev.twilitrealm.randomizer`: 881.
- `com.dusklight.twilit_essentials`: 788.
- Los 22 auditados: cero faltantes contra la superficie actual.

ALBW es la primera gran prueba integral. Si ALBW, Dawnlight y Twilit Essentials funcionan por separado, habrá una base fuerte para la mayoría de mods nativos de gameplay. No cubren por sí solos:

- WebSocket y servicios de red.
- Bibliotecas runtime adicionales arbitrarias.
- Todos los patrones TLS/excepciones.
- Conflictos entre mods que hookean el mismo destino.
- Todos los casos de unload/reload.

Por eso el navegador debe seguir calculando compatibilidad por versión y SHA-256, no declarar que todo mod nativo es compatible por herencia.

### Estrategia de builds del cargador

Objetivo: cuatro checkpoints amplios de hardware, no un NRO por mod.

#### Checkpoint L1: parser y módulo mínimo

- Dry Run completo.
- Segmentos y relocaciones básicas.
- Módulo mínimo y Roll and Sprint en SD.
- Safe mode inicial.

#### Checkpoint L2: hooks y gráficos

- Corregir lo descubierto en L1.
- Roll and Sprint, Deferred Fog/VBAO y ALBW disponibles simultáneamente en la SD, activados uno por uno.
- Registro TLS/unwind si la auditoría lo requiere.

#### Checkpoint L3: mods grandes y lifecycle

- ALBW, Dawnlight, Essentials y Randomizer.
- Enable/disable, cambio de escena, suspensión/reanudación y reinicio.
- Detección de conflictos y rollback del último mod.

#### Checkpoint L4: endurecimiento

- Entradas ELF corruptas/fuzzed.
- Falta de símbolos/servicios.
- Poco espacio/memoria.
- Actualización y reemplazo de `.dusk`.
- Sesión prolongada.

Cada checkpoint usa el mismo NRO para todos los mods de ese nivel. Los `.dusk` se copian o actualizan en SD. Solo se recompila después de agrupar fallos por causa común y aplicar una corrección amplia.

### Servicios de red

HTTP ya funciona. `net` y `websocket` siguen desactivados/stub en Switch. Crests of Courage no es una buena primera prueba del cargador porque puede cargar correctamente y aun así quedar bloqueado por servicios.

Trabajo posterior:

- Backend Borealis `net` sobre sockets BSD/libnx.
- DNS, TCP/UDP, connect/listen y timeouts.
- WebSocket RFC 6455 sobre sockets + SSL, o una versión de curl que proporcione el backend requerido.
- Pruebas separadas de reconexión, suspensión y pérdida de red.

## Entregable F: controles opcionales estilo Wii

### Base que ya existe

H27 ya dispone de:

- Lectura real del sensor de seis ejes mediante libnx.
- Conversión correcta de las unidades de libnx a radianes/s y m/s².
- Gyro Aim para primera persona y objetos compatibles.
- Gyro Rollgoal.
- Sensibilidad, deadband, smoothing e inversión configurables.
- Rumble por libnx.

Los archivos centrales son `extern/aurora/lib/device_switch.cpp`, `src/dusk/gyro.cpp` y `src/dusk/ui/settings.cpp`.

Cuando hay un par de Joy-Con separado, `device_switch.cpp` obtiene dos handles pero actualmente elige solo el derecho. Eso basta para apuntar, pero no para distinguir un Wii Remote de un Nunchuk.

### Perfiles de control

Ofrecer tres perfiles claros:

1. **Tradicional**: botones y sticks; motion completamente desactivado.
2. **Tradicional + Gyro Aim**: comportamiento actual para apuntar y Rollgoal.
3. **Estilo Wii**: dos Joy-Con separados, con mano de espada configurable.

El perfil Estilo Wii es opcional. Todos los gestos conservan un botón alternativo para accesibilidad, Pro Controller, portátil, Flip Grip y situaciones donde un sensor no esté disponible.

### Mano de espada independiente de Mirror Mode

Agregar estas opciones por separado:

- `Mano de ataque por movimiento`: Izquierda / Derecha.
- `Fuente de Gyro Aim`: Derecha / Mano de ataque / Sensor principal.
- `Link zurdo o diestro visualmente`: opción separada y experimental hasta pasar la matriz completa de animación/combate.
- `Mirror Mode`: continúa siendo una opción completamente independiente.

La versión GameCube que ejecuta Dusklight ya muestra a Link zurdo en un mundo sin reflejar. Por eso el modo recomendado puede usar:

- Joy-Con izquierdo: gesto de espada.
- Joy-Con derecho: apuntado, botones y gesto secundario.
- Link: zurdo visualmente, sin Mirror Mode.

También puede permitirse atacar moviendo el Joy-Con derecho sin reflejar el mundo. En la primera etapa la acción funcionará aunque Link siga usando la izquierda visualmente. Cambiar solamente qué sensor dispara el ataque es sencillo y no altera mapas, cámaras, texto ni lógica.

#### Ruta confirmada con Dawnlight Dual Wield

Dawnlight demuestra que no hace falta reflejar el mundo ni preparar un segundo juego completo de animaciones. Su modo Dual Wield alterna ataques entre la espada izquierda nativa y una segunda espada en la derecha. Cuando corresponde atacar con la derecha:

- Intercepta temporalmente los `J3DAnmTransformKey` usados durante el cálculo del modelo de Link.
- Intercambia los tracks de los huesos de ambos brazos (joints 6-9 y 11-14) y refleja localmente las rotaciones/traslaciones del torso y brazos.
- Conserva el blending nativo mediante `initOldFrameMorf`, incluso al interrumpir un combo.
- Coloca la segunda espada en el joint de la mano derecha y mantiene la vaina en la cadera.
- Redirige `setSwordPos` para que punta, base, dirección, estela y contacto correspondan a la hoja derecha.
- Reutiliza la colisión nativa del escudo y solo modifica la cápsula del Shield Attack cuando cruza ambas espadas.
- Usa IK de dos huesos para guardia, desenfundado y enfundado; no usa escala negativa sobre todo Link.

Esto es una base mucho más segura para `Link diestro local`: tomar una versión reducida del wrapper de animación de Dawnlight, mantenerlo activo durante ataques de espada y trasladar espada/escudo, trail e hit positions a los lados opuestos. No conviene importar Dual Wield completo porque su estado, segunda espada, menú e IK de guardia resuelven otro feature.

El trabajo sigue siendo mayor que cambiar el sensor. Para hacerlo bien hay que cubrir:

- Pose/esqueleto y animaciones.
- Sword/shield attachment joints.
- Matrices de armas y equipo visible.
- Hitboxes, sword trails y efectos.
- Algunas cámaras y poses de cutscene.
- Guardia, desenfundado/enfundado, objetos de dos manos, lobo, caballo y estados especiales.
- Cutscenes y rigs no estándar; deben caer al comportamiento nativo si no pasan las comprobaciones de esqueleto.

La compatibilidad de base es favorable: Dawnlight fija como dependencia el commit Dusklight `35cdedced6fb77c128be115429907cf0990e1cc7`, que ya es ancestro de la rama `aleks2`. Además, Dawnlight publica el proyecto bajo CC0 1.0. Se puede adaptar ese código legalmente; aun así se debe acreditar la procedencia en `PROVENANCE.md` y conservar la implementación propia separada del código que se retirará de souldbminer.

Plan de implementación para no mezclar riesgos:

1. **M0 — motion sin cambio visual:** seleccionar Joy-Con izquierdo/derecho y emitir acciones PAD nativas.
2. **M1 — prototipo diestro en laboratorio:** reflejar localmente solo un ataque normal con el wrapper de animación; sin publicar.
3. **M2 — combate básico:** combo normal, finishers, trail y puntos de impacto; comparar daños y alcance contra modo zurdo.
4. **M3 — equipo:** espada/escudo, guardia, Shield Attack, desenfundado, enfundado y objetos.
5. **M4 — estados:** caballo, lobo, agua, escalada, daño, Hidden Skills, cutscenes y mods grandes.
6. **M5 — opción pública:** activar `Link visualmente: Zurdo/Diestro` solo después de pasar la matriz; si un rig no es el humano estándar de 35 joints, fallback nativo.

Así, `Mano de ataque por movimiento` puede entrar antes y no queda bloqueada por M1-M5. Si la opción diestra no alcanza la calidad requerida para 1.0, permanece experimental sin quitar el perfil Wii zurdo.

M0-M4 deben vivir detrás de toggles de runtime en un único NRO de laboratorio. El overlay de diagnóstico debe mostrar mano activa, proceso de Link, gesto emitido, blade/hitbox seleccionada y motivo de fallback. No generar una build distinta por mano, umbral o ataque.

### Arquitectura recomendada

1. Generalizar el backend de sensor para exponer `Right`, `Left` y `Combined/Primary`. Los enums `PAD_SENSOR_GYRO_LEFT/RIGHT` y `PAD_SENSOR_ACCEL_LEFT/RIGHT` ya existen, pero la implementación actual solo atiende los sensores genéricos.
2. Mantener gyro y acelerómetro de cada lado con timestamp propio.
3. Calibrar reposo y orientación al activar el perfil o reconectar un control.
4. Procesar gestos en una capa independiente del actor de Link.
5. Convertir cada gesto reconocido en un evento/pulsación virtual de PAD.
6. Dejar que el juego ejecute la acción normal; no llamar directamente a métodos de combate.

La inyección en PAD es importante: conserva estados, cooldowns, tutoriales, bloqueo durante menús/cutscenes y compatibilidad con mods que observan o reemplazan el input normal.

### Mapeo fiel a Twilight Princess Wii

El manual oficial confirma el núcleo del esquema original:

- Wii Remote: swing suave = corte de espada; swings consecutivos = combo.
- Nunchuk: swing lateral = Spin Attack.
- Mientras cabalga: mover Wii Remote o Nunchuk también ataca con la espada.
- Wolf Link: Wii Remote = mordida; Nunchuk lateral = Spin Attack.
- Nunchuk: `Z` = lock-on/defensa; `C` = primera persona.
- Hidden Skill Shield Attack: empuje corto del Nunchuk hacia delante mientras se hace Z-targeting.
- Apuntado con gyro para resortera, arco, boomerang, clawshot, Dominion Rod y otros objetos ya soportados.
- Pesca y minijuegos de movimiento solo después de estabilizar combate y apuntado.

En Switch, el Joy-Con de espada sustituye al Wii Remote y el Joy-Con opuesto sustituye al Nunchuk. En perfil zurdo recomendado: izquierdo = espada y derecho = Nunchuk/aim. En perfil Wii original/diestro: derecho = espada y izquierdo = Nunchuk. El apuntado por gyro sustituye al puntero infrarrojo que Switch no tiene.

No intentar reconocer dirección exacta del corte en la primera versión: Twilight Princess Wii trataba el swing principalmente como disparador del ataque nativo, no como esgrima 1:1. Primero debe sentirse consistente y no producir falsos positivos.

### Perfil fiel y capa de mejoras

Ofrecer dos presets dentro de Estilo Wii:

- **Wii fiel:** swing de espada, Nunchuk lateral para Spin Attack, empuje frontal para Shield Attack, comportamiento contextual de caballo/lobo y cooldowns cercanos al original.
- **Wii+:** misma base con mano configurable, Gyro Aim moderno, sensibilidad/umbral/cooldown configurables, calibración rápida, rumble al aceptar un gesto y botones alternativos siempre activos.

Mejoras opcionales posteriores, desactivadas por defecto:

- Gesto fuerte o doble swing configurable sin reemplazar acciones obligatorias.
- Umbrales separados para espada, giro y escudo.
- Zona muerta y bloqueo temporal mientras se apunta para evitar disparos accidentales.
- Overlay de diagnóstico que muestre sensor, pico, gesto reconocido y cooldown.
- Modo de práctica sin daño para calibrar gestos.
- Dirección de corte experimental solo si puede seleccionar el ataque nativo sin falsificar el stick ni romper movimiento.

### Diseño de ajustes en la pestaña Aleks

En Switch, colocar una sección nueva `Wii Motion (Aleks)` después de `Companion (Aleks)` y antes de `Diagnostics (Aleks)`. La pantalla principal debe permitir configurar la experiencia completa con pocas filas:

- `Motion Control Style`: Off / Gyro Aim Only / Wii Faithful / Wii+ / Custom.
- `Sword Motion Hand`: Left / Right.
- `Link Visual Hand`: Left / Right / Match Motion Hand. Marcar Right y Match como experimentales hasta M5.
- `Nunchuk Gestures`: Off / Spin Only / Spin + Shield Attack.
- `Gyro Aim Source`: Sword Hand / Nunchuk Hand / Primary Controller.
- `Gesture Strength`: Light / Normal / Strong.
- `Gesture Rumble`: Off / Light / Normal.
- Botón `Calibrate Motion`.
- Botón `Motion Test & Practice`.
- Botón `Advanced Motion Settings`.

Los presets deben aplicar valores coherentes, no solamente cambiar una etiqueta:

| Preset | Configuración |
|---|---|
| Gyro Aim Only | Sin ataques por movimiento; conserva el Gyro Aim actual |
| Wii Faithful | Joy-Con derecho como Remote/espada, izquierdo como Nunchuk, spin lateral y shield thrust frontal |
| Wii+ | Igual que Wii Faithful, pero permite mano, aim source, fuerza y rumble personalizados |
| Custom | Aparece automáticamente al cambiar una opción fuera del preset |

`Advanced Motion Settings` debe abrir una página aparte con umbrales por gesto, cooldown, duración mínima/máxima, smoothing, deadband, rechazo durante Gyro Aim y telemetría. No duplicar los ajustes generales de Gyro Aim que ya existen en `Input -> Gyro`; enlazar o reutilizar esos mismos `ConfigVar`.

`Motion Test & Practice` debe funcionar sin atacar dentro del juego: muestra en tiempo real Joy-Con detectado, aceleración/velocidad angular, gesto candidato, gesto aceptado, cooldown y acción PAD que se habría emitido. También debe permitir probar espada, spin y shield thrust individualmente. Esta pantalla es clave para calibrar en un solo NRO y evitar builds por cada cambio de umbral.

Reglas de UX:

- Al elegir Wii Faithful, mostrar un diagrama corto: Right = Sword/Remote, Left = Nunchuk.
- Si solo hay Pro Controller, portátil o un Joy-Con, explicar qué funciones quedan disponibles en vez de fallar silenciosamente.
- Si `Link Visual Hand` no puede aplicarse al estado actual, usar fallback zurdo y mostrarlo solo en diagnóstico; nunca interrumpir el juego con un aviso.
- Mantener siempre el botón tradicional equivalente a cada gesto.
- Guardar estos ajustes globalmente, no dentro del save de Zelda; el jugador puede usar los mismos controles con cualquier partida.

### Reconocedor de gestos

Usar una máquina de estados, no un único `if (magnitud > umbral)`:

- Filtro de ruido y compensación del reposo.
- Magnitud angular y/o aceleración lineal independiente de orientación.
- Umbral de inicio mayor que el de finalización (histéresis).
- Duración mínima y máxima.
- Cooldown después de aceptar un gesto.
- Rechazo mientras el usuario apunta suavemente.
- Rechazo durante vibración fuerte si contamina el sensor.
- Telemetría opcional con picos, duración y motivo de aceptación/rechazo.

Ajustes mínimos:

- Activar Controles estilo Wii.
- Sensibilidad de gestos.
- Intensidad requerida.
- Mano dominante/intercambio de Joy-Con si se decide soportarlo.
- Gyro Aim independiente.
- Botones alternativos siempre visibles.

### Compatibilidad por tipo de control

| Control | Experiencia prevista |
|---|---|
| Par de Joy-Con separados | Perfil completo; espada izquierda o derecha y sensor opuesto como offhand |
| Pro Controller | Gyro Aim y, opcionalmente, gesto único; no puede representar Remote/Nunchuk por separado |
| Portátil | Gyro Aim; evitar ataques por sacudir toda la consola como valor predeterminado |
| Un Joy-Con/Flip Grip | Gyro Aim y controles tradicionales; gestos limitados y opt-in |

### Pruebas y aceptación

- Cero ataques falsos durante 10 minutos de juego normal con el perfil tradicional.
- Cero activación de gestos cuando el perfil Wii está apagado.
- Latencia perceptible baja: gesto a pulsación virtual en el mismo tick o el siguiente.
- Reconexión y cambio entre portátil/Joy-Con/Pro Controller sin dejar sensores abiertos o cruzados.
- Menús, cutscenes, escalada, natación, lobo y montura no reciben acciones inválidas.
- El botón alternativo siempre funciona aunque el sensor falle.
- Prueba de 20 minutos de combate y apuntado sin drift acumulativo.

Este trabajo puede entrar después de estabilizar rendimiento y saves. No debe mezclarse en el mismo checkpoint que DRS o el cargador ELF, porque un fallo de input sería difícil de aislar.

## Entregable G: retirada del código de souldbminer

### Regla de publicación

Aunque el fork de origen se basaba en código CC0, el autor pidió que no se publicara un fork usando su trabajo. La decisión del proyecto es respetarlo: no se distribuye una versión pública hasta reemplazar el código atribuible y cerrar la auditoría de procedencia.

`PROVENANCE.md` y `HITO6.md` (fuera de este repositorio) son las fuentes históricas; el cierre de H32 está en [switch-h32-provenance.md](switch-h32-provenance.md). El medidor `platforms/switch/tools/souldb-residue.py` compara líneas literales contra su delta y deliberadamente produce un techo, no una acusación exacta de autoría.

Medición ejecutada sobre el árbol actual el 2026-09-29:

```text
3602 coincidencias de un conjunto histórico de 6719 líneas (54 %)
```

Las concentraciones principales del techo actual son:

- `platforms/switch/patches/sdl3-switch.patch`: 1047.
- `platforms/switch/patches/dawn-switch.patch`: 781.
- `platforms/switch/CMakeLists.txt`: 195.
- `platforms/switch/source/borealis_io_switch.cpp`: 188, muchas probablemente firmas obligadas por Borealis.
- Glue de CMake, abseil, Tracy y pequeños deltas de `src/`.

Además quedan dos referencias textuales directas:

- Autor del NACP en `platforms/switch/CMakeLists.txt`.
- Un comentario histórico en `src/dusk/switch_builtin_mods.hpp`.

Quitar nombres o reformatear líneas no reemplaza código. El cierre debe hacerse por subsistema:

1. Definir la interfaz y pruebas de comportamiento.
2. Reimplementar desde upstream, documentación pública o código con permiso explícito.
3. Comparar comportamiento y rendimiento.
4. Borrar el bloque anterior, no conservarlo como fallback.
5. Ejecutar el medidor y auditar manualmente las coincidencias restantes.
6. Actualizar `PROVENANCE.md` con origen y licencia del reemplazo.

### Orden sugerido sin veinte builds

#### P1: parches y toolchain

- Auditar/reemplazar SDL3, Dawn, abseil y Tracy por cambios propios mínimos o eliminar la necesidad del parche.
- Validar compilación, input, ventana, threads y render en pruebas de escritorio cuando sea posible.
- Un checkpoint conjunto en hardware.

#### P2: plataforma y build glue

- CMake, Borealis, POSIX, prelaunch, validación de ISO y metadata.
- Unit tests/host tests para rutas y errores.
- Un checkpoint conjunto en hardware.

#### P3: deltas de rendimiento de Aurora

- Medir cada optimización antes de conservarla.
- El instancing histórico mostró un beneficio despreciable y quedó retirado por completo en H29-c1. La fusión upstream por concatenación de índices se conserva.
- Comparar cache de `ShaderInfo` y ajustes `gfx/` mediante el benchmark automático.
- Un checkpoint A/B, luego borrar definitivamente la variante no elegida.

### Cierre de procedencia

- Medidor en cero o cada coincidencia restante justificada como firma/trivial/upstream y documentada manualmente.
- `PROVENANCE.md` sin sección pendiente.
- Metadata/NACP con créditos correctos de la versión final.
- `git diff` contra upstream y referencias permitidas revisado por archivo.
- No quedan fallbacks compilables del código retirado.

Esta puerta es independiente del rendimiento: una implementación más rápida no puede publicarse si conserva código que se decidió retirar.

## Referencia externa: port privado de Twilight Princess para 3DS

El port mostrado por Game of Tobi es una referencia interesante de viabilidad en hardware limitado, pero **su implementación real todavía no es pública**. El repositorio público `UselessMnemonic/tp3d` es por ahora un fork del decomp original y no expone el backend de 3DS ni sus optimizaciones. No debe usarse como evidencia de técnicas concretas.

### Confirmado públicamente al 2026-09-30

- En el video del 27 de septiembre de 2026, el autor dice que todavía existen problemas de rendimiento notorios que **aún no ha optimizado** (`0:25`).
- Afirma que el 3D estereoscópico ya funciona y que en muchos lugares el rendimiento es muy bueno (`0:25-0:34`).
- Dice que la versión para New 3DS ya funciona muy bien, mientras que el soporte para Old 3DS sigue siendo un problema (`0:34-0:50`).
- El video no identifica cambios de renderer, batching, cachés, streaming, formatos de textura, threading ni flags de compilación.

Por tanto, no hay base pública para afirmar que use Deko3D, una caché especial de shaders, DRS, multithreading concreto o alguna optimización transferible a Switch.

### Qué sí podemos aprender sin adivinar

1. Usar el video como catálogo visual de escenas exigentes, pop-in, pausas y cambios de zona, no como benchmark comparable: 3DS y Switch tienen resoluciones, GPU, API y presupuestos distintos.
2. Esperar a una publicación técnica o al código real antes de copiar arquitectura.
3. Cuando haya fuentes públicas, revisar en este orden: shim de GX/plataforma, traducción de estados y shaders, caché de estado, reducción de draw calls, formatos/residencia de texturas, streaming, asignación de threads y configuración específica de New 3DS.
4. Convertir cada idea encontrada en un cambio medible dentro del NRO de laboratorio y conservarla solo si mejora p95/p99, CPU o memoria frente a H29 a clocks stock.

Esta referencia no cambia la ruta de 1.0: primero observabilidad, luego optimizaciones medidas en Dawn/NVK. Sí queda como seguimiento para reevaluar cuando el autor publique código o un desglose técnico.

## Entregable H: Deko3D descartado para esta ruta

### Decisión

Deko3D no forma parte del roadmap activo. Se mantiene esta nota porque sí puede reducir overhead y ofrecer acceso más directo al Tegra X1, pero no existe evidencia de que Dusklight vaya a duplicar sus FPS ni de que llegue automáticamente a 60 FPS. La cifra debe considerarse una hipótesis del autor del otro port, no un resultado reproducible.

La documentación oficial describe Deko3D como una API explícita y de bajo nivel, con control directo de recursos/comandos y funciones de Maxwell que históricamente no estaban en nouveau. También afirma menor overhead frente a OpenGL. Sin embargo, esa comparación no es una medición de Dusklight y no corresponde exactamente a nuestra ruta actual Dawn -> Vulkan -> NVK.

### Ventajas plausibles

- Menos capas y validación en la ruta de comandos.
- Menor uso de CPU en creación/binding/submission si el render thread es el cuello.
- Control explícito de memoria y render targets.
- Acceso a características específicas de Maxwell.
- Menor tamaño de dependencias y build más rápido al retirar Dawn/NVK.
- Shaders DKSH precompilados, eliminando compilación durante gameplay cuando existe una variante conocida.

### Bloqueo principal: shaders dinámicos

Aurora genera variantes de shader/pipeline a partir del estado GX durante la ejecución. Deko3D utiliza DKSH y su compilador UAM es una herramienta de PC para build-time; oficialmente no puede ejecutarse dentro del programa.

Por eso el plan histórico `PLAN_DEKO3D.md` contiene una suposición que debe corregirse: no existe una ruta simple `SPIR-V -> UAM` en runtime. Antes de construir el backend completo hay que resolver una de estas estrategias:

1. Generar una lista amplia con Shader Tour y compilar todas sus variantes a DKSH durante el build.
2. Construir un paquete de shaders actualizable y aceptar que una variante desconocida no tiene fallback inmediato.
3. Escribir/integrar un compilador JIT a código Maxwell, trabajo de gran tamaño y riesgo.
4. Mantener un backend secundario para variantes desconocidas, lo cual complica recursos, sincronización y compatibilidad.

También debe resolverse qué ocurre con mods que importan `GfxService`/WebGPU. Retirar Dawn puede romperlos aunque el juego base renderice.

### Cuándo podría ayudar

| Cuello actual | Ganancia probable de Deko3D |
|---|---|
| CPU en Dawn/validación/submission | Potencialmente alta |
| Driver/pipeline creation | Alta si todo queda precompilado |
| GPU fragment/fillrate a 900p | Limitada salvo que las funciones Maxwell cambien el costo |
| Lógica/actores/render worker previo al backend | Baja |
| Stutter por shaders | Puede mejorar, pero exige resolver AOT/variantes desconocidas |

### Qué se exigiría para reconsiderarlo

Solo se reabre por una decisión explícita futura y empezando con un spike aislado, nunca con el backend completo:

1. Capturar una escena y su conjunto de pipelines desde el NRO de laboratorio.
2. Compilar offline unas variantes reales a DKSH.
3. Renderizar un workload representativo con texturas, depth, blending y número de draws parecido.
4. Medir CPU submit, GPU frametime, memoria y p95 a clocks stock.
5. Comparar contra Dawn/NVK en el mismo hardware y resolución.
6. Probar que la estrategia de shaders puede cubrir una zona nueva y un mod gráfico.

Criterio sugerido para continuar:

- Ganancia reproducible que cambie una limitación real, no solo un microbenchmark.
- Estrategia cerrada para shaders desconocidos.
- Plan de compatibilidad para `GfxService`.
- Alcance y mantenimiento aceptables.

Sin esas cuatro respuestas, Deko3D permanece descartado. Para 1.0 y el trabajo posterior inmediato se optimiza y mide Dawn/NVK, que ya ejecuta todo el juego y conserva la compatibilidad WebGPU de los mods.

## Secuencia propuesta de trabajo

### Fase 0: congelar la base

- Conservar H28 como línea base anterior y H29-c1 como nueva base física validada.
- Añadir ID de commit/build visible a todos los reportes.
- No mezclar optimizaciones no medidas con el primer NRO de laboratorio.
- Mantener la retirada de souldbminer como puerta de publicación y actualizar la auditoría después de cada reemplazo.

### Fase 1: observabilidad

- Benchmark corto.
- Reporte JSON.
- Matriz de ajustes.
- Estrés DRS.
- Progreso de pipelines.

Resultado: una build permite decidir resolución, efectos y DRS.

### Fase 2: UX y rendimiento de V1

- Auto 900p/720p/540p.
- Shader Tour de desarrollo.
- `initial_pipeline_cache.db` y pantalla Preparando sombreadores.
- Navegador en dos columnas con imágenes presupuestadas.
- Corregir DRS solo con evidencia del estrés.

### Fase 3: cargador dinámico

- Checkpoints L1-L4.
- ALBW como primera prueba integral.
- Dawnlight, Essentials y Randomizer antes de declarar compatibilidad amplia.

### Fase 4: después de 1.0 o si sobra margen

- `net` y WebSocket.
- Cachés descargables.
- Interpolación visual a 60 FPS.
- Controles estilo Wii si no alcanzan a entrar como opción estable en 1.0.
- Experimentos de afinidad del hilo principal.
- LTO/PGO y comparaciones de driver.
- Deko3D queda fuera de la ruta salvo que el usuario decida reabrirlo explícitamente con evidencia nueva.

## Plantilla de cada sesión de prueba

Guardar junto al log:

```text
Build/commit:
NRO SHA-256:
Consola/modelo:
Driver:
Modo: dock / portátil / Flip Grip
Clocks: stock
Resolución/perfil:
Caché: limpia / caliente / seed
Mods activos y SHA-256:
Prueba ejecutada:
Resultado:
Crash/device loss:
FPS avg / 1% low:
p95 / p99 frametime:
Pipelines iniciales/finales:
Memoria antes/después:
Observaciones reproducibles:
Archivos adjuntos:
```

No crear el siguiente build hasta clasificar cada fallo como:

- Código del NRO.
- Paquete/mod.
- Caché fría.
- Red.
- Assets/save.
- Driver/GPU.
- No reproducible.

Agrupar correcciones de la misma categoría en un solo checkpoint.

## Puertas de salida para V1.0

- 30 FPS objetivo medido a clocks stock, con 900p Auto en dock.
- Benchmark repetible y reportes guardados.
- Preparación de shaders reanudable; segunda ejecución caliente.
- Navegador utilizable sin tirones grandes al aparecer imágenes.
- DRS habilitado solo si supera 100 transiciones y 30 minutos sin crecimiento/crash.
- Saves, suspensión y asset mods sin regresión.
- Cross-save supera la matriz de guardado/subida, descarga, conflicto simultáneo, pérdida de red y recuperación en PC, Android y Switch; nunca reemplaza un save sin copia restaurable.
- Auditoría de procedencia cerrada antes de cualquier publicación.
- El perfil tradicional no cambia por añadir motion controls; si el perfil Wii entra, conserva botones alternativos y pasa la matriz de controles.
- Si el cargador dinámico entra en 1.0: Roll and Sprint dinámico más al menos una gran prueba integral estable.
- Si ALBW/Dawnlight no pasan a tiempo, presentar code mods como experimentales y no prometer compatibilidad general.

## Cómo retomar en la siguiente sesión

Orden recomendado para el primer día:

1. Leer este documento y, si se dispone de él, `HITO7_MODS_NATIVOS.md` (no incluido en este repositorio) antes de modificar código.
2. Confirmar HEAD, estado de Git y que H32-c1-b84a849829 siga siendo la base física validada (H31-c1 como fallback). Los pasos 3-5 se escribieron en el corte H29; el análisis de rebuilds ya lo cubrió H31 (memo de pipelines) y el estado actual está en la última sección.
3. Leer los JSON de H28/H29 y analizar primero los `pipeline_rebuilds` y `uniform_rebuilds`; no compilar mientras no exista un candidato concreto.
4. Mantener el benchmark, su esquema 2 y `F_SP122` sin ampliarlos durante el siguiente cambio de rendimiento.
5. Agrupar una única optimización justificada en el siguiente NRO y compararla contra H29-c1; no crear variantes por parámetro.
6. Continuar en paralelo la auditoría de procedencia por subsistemas, sin confundir el medidor literal con autoría real.
7. Después elegir el siguiente bloque de V1 según impacto: preparación de shaders, navegador o DRS.
8. Empezar el cargador dinámico como un hito separado usando los checkpoints L1-L4 y los mismos `.dusk` en SD.
9. Ejecutar la retirada de souldbminer como una línea paralela, con checkpoints P1-P3 y actualización de `PROVENANCE.md`.
10. No iniciar trabajo de Deko3D; está descartado para esta ruta.

Prompt breve de reanudación para otro agente:

```text
Lee docs/switch-v1-roadmap.md, docs/switch-h28-diagnostics.md,
docs/switch-h31-pipeline-memo.md y docs/switch-h32-provenance.md completos. Trabaja sobre el
estado actual de la rama, conserva cambios ajenos y toma H32-c1-b84a849829 como base física
validada (H31-c1 como fallback). No generes una build por ajuste: concentra cambios
relacionados en un único NRO y deja un reporte verificable de cada prueba.
```

## Referencias

- [Dusklight Mod API](https://github.com/TwilitRealm/dusklight/blob/main/docs/modding.md)
- [A Link Between Twilight](https://twilitrealm.dev/mods/dev.albt.albw/)
- [Dawnlight](https://github.com/BeZide93/dawnlight)
- [Dawnlight: Dual Wield](https://github.com/BeZide93/dawnlight/blob/main/docs/features/dual-wield.md)
- [Dawnlight: implementación Dual Wield](https://github.com/BeZide93/dawnlight/blob/main/src/dual_wield.cpp)
- [Dawnlight: wrapper local de animación](https://github.com/BeZide93/dawnlight/blob/main/src/dual_wield_animation.hpp)
- [Dawnlight: licencia CC0 1.0](https://github.com/BeZide93/dawnlight/blob/main/LICENSE.md)
- [Manual oficial de Twilight Princess para Wii](https://csassets.nintendo.com/noaext/image/private/t_KA_PDF/Wii_Legend_of_Zelda_Twilight_Princess?_a=DATAg1AAZAA0)
- [Twilit Essentials](https://github.com/F1mmel/dusklight-twilit-essentials)
- [Khronos Vulkan pipeline cache sample](https://github.com/KhronosGroup/Vulkan-Samples/blob/main/samples/performance/pipeline_cache/README.adoc)
- [devkitPro/deko3d](https://github.com/devkitPro/deko3d)
- [devkitPro/UAM](https://github.com/devkitPro/uam)
- [Game of Tobi: Real Twilight Princess 3DS - 27min Gameplay](https://www.youtube.com/watch?v=7SkmO3y9zjc)
- [Fork público `tp3d` (no contiene todavía el port real de 3DS)](https://github.com/UselessMnemonic/tp3d)
- `extern/aurora/lib/gfx/pipeline_cache.cpp`
- `extern/aurora/lib/webgpu/gpu_cache.cpp`
- `platforms/switch/mod_abi_surface.txt`
- `src/dusk/switch_native_module.cpp`
- `platforms/switch/cmake/StaticMods.cmake`
- `src/dusk/dynres.cpp`
- `src/dusk/ui/mod_browser.cpp`
- `src/dusk/ui/remote_texture_provider.cpp`

## Actualización 2026-10-01: estado tras H30

- Base de desarrollo actual: H30-c1 (Mesa/NVK 26.2.3), validado en hardware; H29-c1 queda como fallback. Línea base de F_SP122 en Flip Grip (caché caliente, una ejecución): 26,69 FPS, p95/p99 53,6/56,0 ms, Execute 35,8 ms, DrawRec 16,3 ms. Detalle en `docs/switch-h28-diagnostics.md`.
- La Fase 1 (benchmark, JSON) está hecha; el instanciado heredado se retiró (H29).
- Entregable B gana prioridad: tras cambiar de driver, la cola de 584 pipelines tardó ~270 s en vaciarse en el primer arranque. La pantalla «Preparando sombreadores» y la lista inicial de pipelines son el siguiente bloque de producto.
- Antes de una V1 pública: sustituir el SDK Mesa reparado con `ranlib` por un paquete corregido y reproducible, o reconstruirlo desde las fuentes públicas.

## Actualización 2026-10-01: estado tras H32

Base de desarrollo y validación física actual: **H32-c1-b84a849829** (commit de código `b84a849829`, Aurora `d3f21d9`), validada en hardware el 2026-10-01. **H31-c1** (`H31-c1-e8a3c8a4b8`) es el fallback de rendimiento y H30-c1 el anterior. Las notas «base H29-c1» y «base H30-c1» de las secciones previas son históricas. Evidencia en [switch-h31-pipeline-memo.md](switch-h31-pipeline-memo.md) y [switch-h32-provenance.md](switch-h32-provenance.md); los hashes de commit de H29-H31 pertenecen al historial original y no existen en este repositorio.

Cifras observadas (una ejecución caliente cada una, F_SP122, 540p): H31-c1 29,27 FPS, Execute 32,3 ms, DrawRec 9,5 ms; H32-c1 29,52 FPS, p95 48,2 ms, Execute 31,9 ms, DrawRec 8,97 ms, memo de pipelines sin misses. H32 es un hito de procedencia, no de rendimiento: la diferencia está dentro del ruido.

### Clasificación del trabajo

| Estado | Contenido |
|---|---|
| Completado y validado en hardware | Benchmark de 60 s con reporte JSON (esquema 2); retirada del instanciado heredado (H29); Mesa/NVK 26.2.3 (H30); memo de pipelines de 1024 entradas (H31); cierre de procedencia de H32 (libro de justificaciones sin pendientes, validación de discos RVZ/WIA/GCZ a través del contenedor, y la sustitución del código heredado documentada en `switch-h32-provenance.md`). |
| Pendiente (planificado, no implementado) | Matriz automática de ajustes y estrés de DRS (los documentos del repositorio no registran que existan); pantalla «Preparando sombreadores» e `initial_pipeline_cache.db` (Entregable B); navegador de mods a dos columnas con imágenes presupuestadas (D); cargador dinámico `.so` con checkpoints L1-L4 (E); resolución automática de 900p en dock (el comportamiento actual validado en H32 es 1080 filas acopladas, 720 en portátil y 540 con Flip Grip); sustituir el SDK Mesa reparado con `ranlib` antes de una V1 pública. |
| Propuesta (diseño, sin implementar) | Cross-save en la nube ([cross-save-1.0.md](cross-save-1.0.md)); controles opcionales estilo Wii (Entregable F, hitos M0-M5); Link zurdo/diestro visual. |
| Investigación futura | Servicios `net`/WebSocket en Switch; cachés de Dawn descargables; interpolación a 60 FPS; afinidad de hilos, LTO/PGO; seguimiento del port 3DS cuando haya código público. Deko3D permanece descartado para esta ruta. |

La retirada de código de souldbminer (Entregable G) quedó cerrada técnicamente en H32 según su documento; la medición de 2026-09-29 citada arriba (54 %) es previa y solo histórica. La decisión de publicar sigue siendo independiente del rendimiento y corresponde al responsable del proyecto.

## Actualización 2026-10-02: H34 (sin validar en hardware)

Base: H33-c1 (`52076ba037`, cierre `7350859524`), validado en hardware; H32-c1 queda como fallback. H34 reúne en un
solo candidato el selector de disco de Switch, la corrección de los tirones del navegador de mods (decode fuera del
hilo principal, prioridad visible, publicación presupuestada) y **Prepare Shaders** (recorrido de seis zonas que llena
las cachés propias de Aurora en la consola; no distribuye ni descarga cachés). Detalle, evidencias y plan de prueba en
[switch-h34-content-preparation.md](switch-h34-content-preparation.md). La auditoría del FIFO para H35 está en
[switch-h35-fifo-plan.md](switch-h35-fifo-plan.md). `tools/make-shader-seed.py` no se reconstruye: solo serviría para
distribuir una semilla en el NRO, descartado en H34.

## Actualización 2026-10-02: H34-c2 validado; H35 (sin validar en hardware)

H34-c2 (`H34-c2-7f2c328c9f`) se probó en hardware y sus tres correcciones funcionan (miniaturas, `config_json_written`,
logros); el benchmark en portátil queda en 29,3–29,4 FPS en los tres perfiles, también a 540p, así que la escena está
limitada por CPU/FIFO. Detalle en [switch-h34-content-preparation.md](switch-h34-content-preparation.md).

H35 (ramas `claude/h35-fifo-tour` y `aurora-aleks-nx` `claude/h35-fifo-uniforms`, un solo NRO de laboratorio, sin
compilar todavía): corrige la entrada de Ordon de Prepare Shaders (punto 1); mide el trabajo del hilo FIFO, los drenajes
por sitio, los publishes y despertares, las esperas de slot y staging y el render worker (informe del benchmark en
esquema 4); añade la reutilización del bloque de uniforms con tres modos en ejecución (Off por defecto, Verify, On); y
añade **Run Performance Tour** (pesada → media → ligera, un JSON combinado) para ver el comportamiento del interpolado
por clase de zona. No promete 60 FPS. Diseño, auditoría, pruebas y protocolo de prueba en
[switch-h35-fifo-plan.md](switch-h35-fifo-plan.md).
