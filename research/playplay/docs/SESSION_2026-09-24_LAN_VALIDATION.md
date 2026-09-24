# Bitácora inicial: conectividad y relectura — 2026-09-24

**Documento histórico del comienzo de la sesión.** Los bloqueos siguientes
fueron superados después. Estado final/evidencia:
[VALIDATION_148_2026-09-24.md](VALIDATION_148_2026-09-24.md).
No usar este archivo como instrucción de dejar otra captura esperando.

## Relectura tras cambios de otro agente

Se leyeron GEMINI, README, PLAN, task y notas técnicas. La carpeta indicada por
el usuario, `research/playplay-lan-deobfuscator/`, contenía solo instrucciones;
el proyecto operativo está en `research/playplay/`.

Durante la revisión había cambios externos que declaraban Hito4 completo y
once vectores correctos, mientras PLAN aún pedía comprobarlos. No se encontró
un log que acreditara esa afirmación. El usuario pidió a ese agente revertir y
se releyeron documentación y C++ después de la reversión.

Estado observado entonces y conservado al cierre:

- `FileProvider.cpp`: FORCE PLAYPLAY y parseo manual find/substr; RPC ya cableado.
- `SpClient.cpp`: token E/v5 y escritura a `/tmp/creds.json`.
- `Session.cpp`: hilo detached de prueba con track fijo.
- Arreglos HMAC preexistentes presentes.
- Hito E2E sin prueba independiente; no se toma el éxito transitorio como evidencia.

La observación anterior del parser tao::json pertenece al estado revertido.
No describirlo como implementación actual ni atribuir la extracción AES al
parser. Esta sesión no modificó los archivos C++.

## Bloqueo inicial de transporte

SSH con `~/.ssh/win_claude`, BatchMode/ConnectTimeout8, a
`francisco.herrera@10.16.150.154` terminó timeout22. La ruta inicial salía
por192.168.100.1/wlp0s20f3; vpn0/vpn1 estaban DOWN/NO-CARRIER. Tras activar VPN
persistió un timeout, y el usuario confirmó PC encendida e IP. Estas mediciones
son históricas; posteriormente se recuperó acceso y hubo ejecución remota real.

La primera prueba HTTP falló por transporte:
[lan-validation-2026-09-24.json](lan-validation-2026-09-24.json).
**Un timeout no prueba incompatibilidad token/build.**
En esa primera fase no se había adjuntado Frida ni creado procesos remotos;
esa frase ya no describe toda la sesión posterior.

## Herramienta y controles locales

Se agregó `tools/validate_lan_service.py`, cliente stdlib para once pares E,
repetición configurable, informes exclusivos, distinción entre mismatch y error,
sin credenciales ni proxies del entorno/redirecciones.

Cuatro tests con servidor simulado pasaron. Prueban validación de respuestas y
manejo de errores, no el VM. Comandos y comportamiento actual:
[tools/README.md](../tools/README.md).

## Observaciones que orientaron la continuación

Se inspeccionó `validate_all_in_js.py`: adjunción a varios procesos, errores
silenciados y `sys.exit()` desde callback mientras el hilo principal dormía;
no era un validador fiable. Se construyó un runner con PID explícito, plazo,
reporte y detach.

El servidor guarda `args[3]` como puntero; la vida de esa memoria y su uso
posterior por RPC eran cuestiones pendientes. Los diagnósticos nuevos copian
sus16 bytes; runtime fresco ofrece una segunda ruta. No se atribuye el
HTTP500 al puntero sin prueba: la corrección comprobada para ejecutar fue
propagar excepciones nativas; además el buffer final no es AES validada.

La continuación recuperó Windows, reprodujo HTTP500, obtuvo controles naturales
del candidato y del generador, y dejó fixtures para repetir sin otra canción.
Todas esas fases, incluidos intentos fallidos e informes, están en
[la bitácora consolidada](VALIDATION_148_2026-09-24.md).
