# API HTTP de extracción AES16 — 2026-09-25

## Objetivo y responsables

Exponer el DFA implementado como API Windows y conectar el cliente cspot,
conservando ambos campos de licencia. Coordinador: servicio Python/JS,
operación exclusiva Windows, documentación y evaluación. Agente
`cspot_api_client` (gpt-6-sol, medium): cliente C++, parser, pruebas y build.
No se enviaron AES de referencia ni credenciales de cuenta a Windows.

## Entradas y fuentes

Build Windows 1.2.92.148; SHA256 de disco verificado por el preflight:
`7b44456a90142daeb758e2736d8628ffe211d6ea523db8f1050b3c8b1addb68a`.
PID inicial34308; después de reabrir Spotify, PID68160 en sesión gráfica2.
Son datos históricos, no valores de configuración.

Licencias guardadas:

- `research/playplay/docs/token148-license-20260924T164814Z-retry.json`.
- `research/playplay/docs/token148-license-second-20260924T164814Z.json`.

Se enviaron solo `obfuscated_key` y `b4_seq`. Referencias y prefijos CDN
permanecieron en Linux. Fuentes exactas finales en `source/final/`.
`source/initial_playplay_dfa_service.py` conserva la primera versión probada;
RPC, preflight y matemáticas son los mismos archivos que la versión final.

## Resultados

| Control | Evidencia / resultado |
|---|---|
| Primera API en Windows | `raw/http-first.json`: 4/4 HTTP200, AES correcta y CRC Ogg/Vorbis |
| Tarea inicial con wrapper | `raw/http-scheduled-final.json`: 4/4; luego se detectaron procesos residuales, por lo que esa forma de arranque se reemplazó |
| Spotify recién abierto | `raw/http-fresh-process-retry.json`: 4/4, aproximadamente1,4s por petición |
| Tras parada ordenada y nuevo arranque | `raw/http-after-graceful-restart.json`: 2/2, mismo Spotify PID68160 |
| Copia Windows final | `raw/deployment-final.json`: 10/10 hashes iguales, una escucha8765, un servidor y un worker, tarea Running |
| Pruebas Python locales | `raw/local-tests.txt`: 8 tests, HTTP inválido/auth, timeout, exclusión de puerto y recuperación real offline |
| Pruebas Python Windows | 7 tests HTTP/worker/puerto pasaron; no se copió la referencia AES para ejecutar el test offline allí |
| Cliente cspot | `cmake --build build --target cspot_cli -j4` completó; helper C++ comprobado por agente |

Las AES recuperadas descifran prefijos de4096B; no se guardan las claves en los
informes HTTP. Se verificaron tanto igualdad con referencia como contenido/CRC.
El servidor exige `b4_seq`; omitirlo devolvió400 sin llamar al extractor.
La suite doctest no pudo ejecutarse porque faltan sus submódulos y trompeloeil;
se conservó la configuración de build con tests deshabilitados.

## Negativos preservados y correcciones

- Al detener la tarea que ejecutaba PowerShell quedaron Python/worker residuales.
  Se observó además un fallo Windows de Spotify34308 en `frida-agent.dll`,
  excepción `0xc0000409`; no se aisló causalidad exacta. Se cerraron únicamente
  los procesos del servicio identificados y se reabrió Spotify por una tarea
  temporal en la sesión gráfica, retirada después de arrancarlo.
- La tarea final ejecuta Python directamente. El puerto se reserva con acceso
  exclusivo antes de adjuntar Frida; una segunda instancia es rechazada.
- `stop_playplay_service.ps1` solicita parada ordenada mediante un marcador local.
  Python espera al request activo, descarga Frida y se desadjunta. Se comprobó
  que no quedaron workers y Spotify68160 siguió vivo antes del nuevo arranque.
- `raw/http-fresh-process.json` conserva un fallo de transporte: el túnel SSH
  local se había cerrado. Se restableció antes del retry positivo.
- `raw/deployment.json` conserva una evaluación fallida del primer colector:
  PowerShell no produjo hashes al recibir strings por pipe en Get-FileHash.
  Se corrigió a `-LiteralPath`; `deployment-final.json` tiene los10 hashes reales.

## Operación final y límites

Tarea a demanda `CspotPlayPlayDfa`, usuario gráfico actual. Fuentes remotas en
`C:/Users/francisco.herrera/playplay-service-20260925/`.
Escucha `127.0.0.1:8765` en Windows; cspot accede por túnel SSH local8765.
La tarea y el túnel quedan activos para el uso solicitado. No se abrió firewall
ni un endpoint público. No quedan capturadores temporales ni tareas de arranque
de Spotify creadas para la prueba.

Se comprobó el código actual con licencias guardadas y con un proceso Spotify
nuevo. No se probó una licencia nueva fuera de esos recursos, reproducción
completa de cspot, ESP32 ni otro build. Un timeout mata el worker, no Spotify,
pero no garantiza abortar una llamada nativa en el proceso instrumentado.

## Reproducir

Seguir [la guía de operación](../../docs/HTTP_DFA_SERVICE.md). Evaluación desde
la raíz del repositorio, usando un informe nuevo:

```sh
python3 research/playplay/tools/verify_playplay_service.py \
  --license research/playplay/docs/token148-license-20260924T164814Z-retry.json \
  --license research/playplay/docs/token148-license-second-20260924T164814Z.json \
  --report /tmp/playplay-http-nuevo.json
```

El colector `source/record_deployment.py` consulta por SSH los hashes de los
archivos desplegados y `/health`; admite `--report` para conservar una captura
nueva. No altera el entorno remoto.
