# Plan vigente — extracción AES PlayPlay en LAN

**Corte: 2026-09-24.** Este archivo manda sobre `task.md`. Las etapas históricas
que declaraban «token incompatible» o «servicio resuelto» fueron sustituidas por
el estado respaldado por los informes de esta sesión.

## Objetivo

Convertir la licencia de PlayPlay que obtiene cspot en una AES de contenido
validada, usando una PC de la LAN. Contrato deseado: entrada `obfuscated_key`
(16 bytes), más `file_id` si hace falta; salida AES (16 bytes). El prototipo HTTP
actual solo manda `obfuscated_key`; aún no implementa todo ese contrato ni caché.
No se ha probado que el proceso Windows pueda inicializarse sin sesión de usuario.

## Lo conseguido

- Ejecución nativa dentro de Spotify 1.2.92.148 con excepciones propagadas.
- Dos rutas para ejecutar el VM: snapshot de 144 bytes restaurado por llamada,
  o constructor interno `0x4a0268` con solicitud local y callback deshabilitado.
- Candidato estable en RDX de `0x49f854`, control natural positivo, 11 vectores
  repetidos dos veces: 0 coincidencias con AES conocidas.
- Salida real de 28 bytes seguida hasta callbacks de precarga y reproducción.
- Inicialización `0xd9e2e4` y generador `0xd9d0f0`: 13 casos × 2, cuatro bloques
  idénticos por repetición. No coincide la comparación con AES/IV estándar.
- Control independiente del generador: snapshot `0x2e4` reproduce un bloque
  natural de reproducción de Windows. Es una prueba de ejecución, no de AES.

## Paso inmediato: conectar el generador nativo con una AES conocida

1. Reproducir el control existente con `check_key_pipeline_148.js` y los fixtures.
   Leer `events`; el runner conserva un resumen pensado para los 11 vectores AES
   y devuelve 2 para estos diagnósticos aunque terminen correctamente.
2. Estudiar `0xd9e2e4` y el estado que consume `0xd9d0f0`: origen del contador,
   significado del auxiliar de 32 bits, representación de clave y tablas.
   Usar como ancla el control natural positivo; no llamar AES a los primeros
   16 bytes de ninguno de esos contextos sin demostrarlo.
3. Diseñar una prueba positiva independiente: asociar inequívocamente una
   licencia/recurso con un chunk cifrado y la posición del contador, o ubicar
   una representación AES que reproduzca bloques de contenido conocidos.
   La caché sondeada hasta ahora no tiene esa asociación ni formato confirmados.
4. Comparar la AES así demostrada con los 11 vectores, separando v2/v3/v4/v5.
   cspot usa v5 y hay solo **dos** vectores v5. Los fixtures no preservan
   `b4_seq`; no atribuir una diferencia a token/build sin revisar el contrato.

**Criterio de cierre de esta etapa:** extracción de 16 bytes respaldada por un
control de contenido/criptográfico independiente y resultados por versión,
repetibles. No basta con que dos invocaciones den el mismo valor.

## Decidir compatibilidad solo después de validar la extracción

- Si los vectores de la versión objetivo pasan, conservar token E/build 148.
- Si una extracción con control positivo falla para E, investigar par token/version
  usado por Windows 148 o el contexto adicional que falte.
- Capturar ese par por instrumentación localizada o, si es necesario, el MitM
  histórico. La sesión actual capturó **respuestas**, no el token/version de request.
- Cambiar a otro build es una alternativa, no el siguiente paso por defecto.
  667 existe localmente, pero no tiene RVAs ni compatibilidad con E validados aquí.
- Unicorn queda como alternativa portable; el intento aislado de 148 carece de
  resultado conservado. Fallar con RVAs de otra sub-build no descarta la emulación.

## Servicio LAN, después de resolver AES

Revisar/reemplazar `resources/flask_frida_server_final.py`: conserva excepciones
por defecto, un puntero inicializador vivo y lee 16 bytes del buffer final.
El ensayo real dio HTTP 500. Su nombre «final» no indica madurez.

Validar primero el algoritmo y después dos pasadas HTTP con
`tools/validate_lan_service.py --repeat 2`. Evaluar serialización de llamadas,
vida de memoria, reinicio del proceso, control de build/hash, contrato y errores.
Mantener el servicio en LAN; no publicar un endpoint sin control de acceso.

## Integración cspot, después de la validación del servicio

Presentar al usuario el plan concreto antes de modificar C++:

- Retirar `FORCE PLAYPLAY` y conservar el fallback ante fallo de la vía legacy.
- Eliminar el volcado a `/tmp/creds.json`.
- Reemplazar el parseo manual del JSON por un parser y validar exactamente 16 bytes.
- Retirar el hilo detached de prueba con track fijo.
- Probar licencia fresca → RPC → chunk CDN con AES-CTR → `OggS`/Vorbis → ESP32.

La prueba histórica de audio con otras claves no acredita este E2E PlayPlay.
Los arreglos HMAC preexistentes se revisan por separado; no son prueba de AES.

## Alternativas ya acotadas

El `LoadLibrary` standalone ensayado falló por entorno/dependencias; no retomarlo
sin una hipótesis concreta. El algoritmo estático gen-B no pasó los vectores.
Cambiar un parser C++ no corrige la extracción criptográfica no validada.
Detalles, alcance y criterios para reabrir: [DECISIONS_148.md](docs/DECISIONS_148.md).
