# Cross-save en la nube para 1.0

> **Estado: diseño y plan para 1.0. Nada de lo descrito en «Diseño» y «Trabajo por fases» está implementado**: no existen el mod, la interfaz de snapshots del host, el servicio remoto ni la cuenta. Solo describen código existente las secciones que lo dicen expresamente («Hallazgos del código actual»). Las cuotas y condiciones de Firebase y Cloudflare son las del momento en que se redactó el documento y no se han vuelto a verificar; deben revisarse antes de decidir. Base de Switch al 2026-10-01: H32-c1-b84a849829 (validada en hardware); este trabajo no forma parte de ella.

## Objetivo

Permitir que una persona continúe la misma partida de Twilight Princess entre PC, Android y Switch mediante un mod oficial de Dusklight. El juego sigue guardando en su formato normal; el mod sincroniza snapshots completos de la tarjeta de memoria.

La sincronización debe ser automática cuando hay red, funcionar sin conexión y pedir una decisión cuando hay dos versiones locales/remotas que no descienden una de otra. Nunca se combinan ni se reemplazan silenciosamente partidas divergentes.

## Hallazgos del código actual

- La partida activa de Switch usa almacenamiento GameCube y produce un `.gci`; el flujo probado persiste y vuelve a leer ese archivo. El mapa de ese flujo (`SAVE_FLOW_MAP.md`) está en el repositorio auxiliar de trabajo `dusklight-switch`, que no forma parte de este repositorio.
- El gestor reciente ya ofrece transferencias y recuperación: la ventana de Saves importa `.gci`, `.dusksave` y tarjetas raw, permite elegir saves de una tarjeta, confirma reemplazos, conserva backups, permite conservar o limpiar datos de mods y exporta con o sin esos datos. `save_manager.*` valida GCI, escribe atómicamente y vuelve a montar la tarjeta.
- Los arreglos recientes de importación permiten cambios de región/plataforma con aviso y reescriben el ID del juego completo. Cross-save debe reutilizar esas mismas reglas y pedir confirmación cuando una importación necesita conversión. Commits del historial original que documentan esta línea (no existen en este repositorio, que parte de un único commit raíz): `17eb5b3749`, `422d7bb1b6`, `cc81157fb8` y `97ca92a056`.
- `SaveService` (`sdk/include/mods/svc/save.h`) observa slots y lee/escribe blobs privados del mod. No expone los bytes de la partida. Por eso el mod no debe buscar el `.gci` con rutas inventadas ni escribirlo directamente.
- `HttpService` permite peticiones HTTPS asíncronas. `FileService` ofrece selectores de archivo en escritorios y Android, pero el selector está desactivado para Switch y no hace falta para sincronizar en nube.
- Android y escritorio compilan mods nativos por arquitectura. Horizon no carga `.so` dinámicamente: `StaticMods.cmake` compila el código del mod dentro del NRO y deja sus recursos en la SD.
- Switch compila un backend HTTP propio para Borealis sobre libcurl/mbedtls y levanta sockets desde `switch_net.cpp`; la opción `ALEKS2_SWITCH_HTTP` está activa por defecto. El trabajo es comprobar que `HttpService` queda disponible al mod estático y probar HTTPS de extremo a extremo en hardware con el endpoint de cross-save.
- El mod Lua no sirve como implementación común: Luau no expone red ni filesystem. La implementación de sincronización será nativa, con UI compartida vía servicios de mod.

## Diseño

### Límites entre mod y anfitrión

El mod oficial contiene la interfaz de cuenta, política de sincronización, cliente HTTPS y estados de conflicto. Usa servicios versionados del host y reutiliza el formato y las validaciones de la ventana Saves. El anfitrión expone una interfaz de snapshots que envuelve las operaciones actuales para:

1. Identificar el save GCN compatible con el disco configurado.
2. Capturar un snapshot consistente solo después de terminar una escritura del juego y cuando la tarjeta no esté ocupada.
3. Validar un snapshot recibido con las reglas existentes de `disc_compatibility`, incluidos región/plataforma, nombre e ID de juego; pedir confirmación para las conversiones que hoy requieren “Import Anyway”.
4. Crear un backup local antes de aplicar, escribir de manera atómica, invalidar los datos en memoria y volver a montar la tarjeta.
5. Devolver un resultado explícito si la tarjeta está ocupada, desmontada, el save es incompatible o falla la escritura.

Se debe ampliar el servicio existente de saves con operaciones de snapshot o añadir un servicio dedicado. No se debe sobrecargar el blob privado de `SaveService`: ese blob es por mod y por ranura y no contiene la partida normal. El servicio de anfitrión también debe suministrar una revisión local persistente que el mod pueda asociar con la cuenta y la identidad del save.

PC/Android cargan el paquete nativo de su ABI. En Switch, el mismo código fuente entra en `ALEKS2_STATIC_MODS`; sus recursos y `mod.json` se distribuyen con el NRO. El empaquetado cambia por plataforma y la lógica de sincronización se comparte.

### Reutilizar la ventana Saves

La importación/exportación manual ya resuelve gran parte del trabajo local y queda como herramienta de recuperación. Cross-save añade sincronización, no otro gestor: el host prepara el mismo artefacto `.dusksave` y lo aplica usando los mismos parser, backups, compatibilidad y escritura atómica. La aplicación automática puede proceder sin el diálogo manual solo si el juego/plataforma es compatible sin conversión y el control de revisión confirma que el save local no cambió. Si hace falta convertir o hay conflicto, se muestra confirmación y se conserva un backup.

### Identidad y formato

- La unidad sincronizada en 1.0 es el GCI completo, que preserva juntos los tres slots. No sincronizar cada slot por separado.
- Clave lógica: juego, maker/región y nombre de save. Rechazar otro juego o nombre; permitir solo cambios de plataforma/región que la política de compatibilidad haya aprobado explícitamente.
- Calcular SHA-256 sobre los bytes exactos del snapshot. El timestamp del sistema o del encabezado GCI no decide cuál partida gana.
- El contenedor `.dusksave` puede llevar los sidecars de mods soportados y metadatos/versionado, pero el backend trata el contenido como bytes opacos. La versión 1 del contenedor ya existe en el gestor actual.
- No incluir datos del disco, assets ni ajustes globales en el snapshot.

### Sincronización y conflictos

Cada dispositivo conserva localmente el último hash aceptado y la revisión remota que confirmó; el servidor mantiene snapshots inmutables y un número de revisión creciente por cuenta e identidad de save. La hora del archivo y el reloj de cada consola solo sirven para mostrar información: no determinan cuál progreso gana.

- **Misma revisión:** si el hash local coincide con el último hash aceptado y el servidor no cambió, no se transfiere nada.
- **Cuenta sin snapshot remoto:** subir el save local tras confirmar la cuenta.
- **Dispositivo nuevo con snapshot remoto y sin save local:** descargar, validar, respaldar/importar y luego registrar el hash.
- **Local igual a la revisión conocida y remoto avanzado:** descargar el remoto después de respaldar el local.
- **Local avanzado y remoto igual a la revisión conocida:** subir el local con compare-and-swap de revisión.
- **Local y remoto avanzaron por separado:** conservar ambos; mostrar fecha, dispositivo y juego, permitir elegir una copia o cancelar. Antes de aplicar, guardar backup local. Después de elegir, actualizar cabeza con una nueva revisión.
- **Sin conexión o error de transporte:** mantener la partida jugable y la cola pendiente; reintentar con límite y backoff. No bloquear guardado, salida ni arranque del juego.
- **Cierre durante subida/descarga:** reanudar desde un objeto temporal o repetir una transferencia pequeña; una cabeza remota solo cambia al completar y validar el objeto.

La subida se programa tras el callback de save escrito y un breve debounce para agrupar escrituras próximas. Al volver al título/iniciar una sesión y al detectar reanudación de red, consultar la cabeza remota. No inspeccionar ni leer el save cada frame.

### Ajustes que ve el jugador

- **Sincronización en nube:** desactivada hasta que el jugador vincule explícitamente una cuenta/dispositivo; después puede desactivarse sin borrar saves locales.
- **Automática / manual:** automática es el valor recomendado después de vincular. Manual mantiene un botón “Sincronizar ahora”.
- **Solo Wi-Fi:** preferencia en Android; Switch y PC sincronizan cuando hay conexión.
- **Si hay conflicto:** “Preguntarme” siempre; no se ofrece una opción que descarte la otra copia sin confirmación.
- Mostrar estado, dispositivo/revisión remota y última sincronización. La fecha es informativa; el hash y la revisión remota determinan qué cambió.

### Servicio remoto y cuenta

Hace falta un endpoint HTTPS mantenido para Dusklight o una especificación compatible autoalojable. El contrato mínimo necesita crear/renovar sesión, listar revisiones, descargar snapshot y subir snapshot de forma condicional. Las escrituras requieren control de revisión (ETag/If-Match o equivalente), límites de tamaño, validación, rate limits y borrado/exportación de cuenta. Cifrar el transporte y el almacenamiento; no registrar tokens ni bytes de saves en logs.

La interfaz del backend debe permanecer separada del mod para permitir cambiar proveedor. Antes de cerrar 1.0 hay que definir quién opera el servicio, política de retención/cuotas y recuperación de cuenta. El host no ofrece hoy un almacén de credenciales seguro común a Windows, Android y Switch; el diseño de inicio de sesión debe decidir cómo emitir y guardar tokens por plataforma, con caducidad/revocación, antes de integrar cuentas reales. No guardar contraseñas dentro del mod.

### Nube sin pagar infraestructura

La ruta recomendada para empezar es **Firebase en el plan Spark**, sin enlazar una cuenta de facturación. El mod llama directamente por HTTPS a [Firebase Authentication REST](https://firebase.google.com/docs/reference/rest/auth) y [Cloud Firestore REST](https://firebase.google.com/docs/firestore/use-rest-api); no hace falta pagar ni mantener un servidor propio o una función de servidor. Firestore acepta tokens de Firebase Auth y aplica [reglas de seguridad](https://firebase.google.com/docs/firestore/security/get-started) por usuario. La API key del proyecto es pública por diseño; las reglas y los tokens, no la ocultación de esa key, protegen las partidas.

Las [cuotas gratuitas actuales](https://firebase.google.com/docs/firestore/pricing) de Firestore incluyen 1 GiB de datos, 50.000 lecturas y 20.000 escrituras al día, y 10 GiB de salida mensual. El plan Spark no pide método de pago para empezar y detiene la función que llega a su cuota en vez de cobrar excedentes. Mantener el proyecto en Spark y no vincular Cloud Billing: Firebase indica que vincular facturación cambia el proyecto a Blaze. Al agotarse una cuota, la sincronización queda pausada hasta que se restablezca; el juego sigue con su save local.

Un [documento de Firestore admite hasta 1 MiB](https://firebase.google.com/docs/firestore/quotas). El `.gci` normal cabe holgadamente; para `.dusksave` mayores, guardar el snapshot en fragmentos de documentos con un manifiesto y publicar la revisión solo cuando todos los fragmentos estén escritos. Limitar la retención remota a un número pequeño de revisiones y dejar que la persona exporte una copia local. Las cuotas, el plan gratuito y la disponibilidad pueden cambiar; no prometer almacenamiento ilimitado ni SLA.

Esto deja un camino de lanzamiento sin factura, a costa de que cross-save deje de estar disponible si se supera la cuota o Firebase cambia el plan. El flujo offline y la importación/exportación local siguen siendo la recuperación.

### Alternativa: API propia de Dusklight en Cloudflare

Si queremos que el jugador hable con una API bajo control de Dusklight, desplegar un Cloudflare Worker como API HTTPS, D1 para cuentas/revisiones y R2 Standard para snapshots. El Worker autentica cada petición, aplica cuotas y retención, coordina compare-and-swap de la revisión y solo publica el snapshot después de validar sus fragmentos. El mod nunca contiene credenciales administrativas de Cloudflare.

Para no implementar y mantener contraseñas propias, se puede conservar Firebase Auth como proveedor de identidad y hacer que el Worker valide sus tokens. Eso separa el login del almacenamiento, pero añade dos proveedores. Usar solo Cloudflare requiere diseñar autenticación, recuperación de cuenta, revocación de dispositivos y protección contra abuso; no recomiendo inventar ese sistema para 1.0.

Cloudflare Free da 100.000 solicitudes Worker al día y D1 Free incluye 5 GB, 5 millones de filas leídas y 100.000 escritas por día. R2 Standard incluye 10 GB-mes, 1 millón de operaciones A y 10 millones B; el egreso no se cobra. Worker y D1 rechazan trabajo al alcanzar sus límites gratuitos, pero el exceso de R2 sí tiene tarifas; poner topes duros por cuenta y por snapshot, además de alertas, para reducir el riesgo de cargo. Si “cero cobros posibles” es requisito, Firebase Spark directo es la opción más segura; si queremos API bajo marca/control de Dusklight y aceptamos operar límites, Cloudflare es la más adecuada.

El mod debe consumir una interfaz HTTPS de proveedor-neutral. En 1.0 se puede conectar directamente a Firebase o a `api.dusklight...`; el formato de snapshot, hashes, revisiones, conflictos y reglas de import permanecen iguales.

## Trabajo por fases

1. **Cerrar contrato:** API de snapshot del host, esquema de identidad, revisiones remotas, proveedor de cuenta y política de retención.
2. **Reutilizar el gestor:** dividir la lógica reusable de `save_manager` para que la llamen la UI nativa y el servicio del host; no duplicar parser GCI, backup, compatibilidad ni escritura atómica.
3. **Crear el mod:** UI de estado, vinculación de cuenta, auto-sync, preferencias de Wi-Fi/datos, selección de conflicto y diagnóstico sin datos privados.
4. **Portar y empaquetar:** las plataformas de PC incluidas en Dusklight 1.0, Android arm64 y Switch estático. Verificar HTTP/TLS real en Switch.
5. **Probar el recorrido:** PC → Android → Switch → PC con una copia de prueba, cada cambio hecho mediante un guardado normal.

## Criterios de aceptación de 1.0

- Una partida GCN continúa en ambos sentidos entre PC, Android y Switch; se comprueban progreso visible y hash del snapshot transferido.
- Guardar sin red conserva progreso y sincroniza después de reconectar.
- Dos dispositivos que avanzaron offline presentan conflicto y mantienen recuperables ambas versiones.
- Fallos inyectados al descargar, validar, respaldar, escribir y remontar no dejan una partida parcial ni destruyen la anterior.
- La sincronización nunca lee un save durante una escritura, nunca reemplaza sin backup y no bloquea el hilo del juego mientras espera red.
- Tokens, saves y datos de cuenta no aparecen en logs ni reportes de diagnóstico.
- Iniciar sesión, cerrar sesión, cambiar de cuenta y borrar datos sincronizados tienen comportamiento definido y se prueban en todas las plataformas.
- Las notas de lanzamiento describen el servicio remoto, requisitos de red, límites de cuenta y recuperación de conflictos.

## Pendientes antes de implementar cuentas

1. Elegir Firebase Spark directo para impedir cargos al exceder cuota, o Worker + D1 + R2 con Firebase Auth para tener una API propia bajo control de Dusklight.
2. Definir mecanismo de login y almacenamiento de credenciales en Switch.
3. Confirmar la matriz de PC soportada por Dusklight 1.0; el plan de Switch por sí solo no define Windows/macOS/Linux.
4. Confirmar la política de datos auxiliares. `.dusksave` ya incluye sidecars de mods; recomiendo sincronizar los sidecars versionados de mods instalados en ambos lados y mantener exportación `.gci` para el progreso vanilla.
