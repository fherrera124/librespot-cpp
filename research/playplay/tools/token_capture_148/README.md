# Captura externa del token148 — material conservado

Origen: seis scripts aportados por el agente externo, antes ubicados en la raíz
PlayPlay. [Nota original](../../docs/external/extraction_kPlayPlayToken_148.md).
No constituyen el capturador limpio del próximo experimento.

| Pareja | Propósito histórico |
|---|---|
| run_scan_proto2.py / scan_proto2.js | Busca encabezado protobuf de versión5/token16 |
| run_scan_loop.py / scan_playplay_loop.js | Busca la cadena de endpoint en memoria |
| run_wsasend.py / hook_wsasend.js | Captura llamadas de envío; tráfico TLS ya cifrado |

Los tres Python ahora resuelven su JS con `Path(__file__)`, independientemente
del cwd. Los JS conservan sus bytes. No se ejecutaron en esta reorganización.

Antes de reutilizar: leer el código. Los runners intentan adjuntar a todas las
instancias Spotify, algunos silencian errores y no registran build/hash por
coincidencia. Adaptar a PID principal verificado y a captura con procedencia;
no interpretar una coincidencia aislada como prueba de petición efectiva.
No ejecutar simultáneamente con otros hooks. Requieren Windows, Frida y psutil;
si se copian, mantener cada Python junto a su JS.

El uso histórico `python run_scan_proto2.py` de la nota asume esta carpeta como
cwd tras la reorganización. La ruta actual es
`research/playplay/tools/token_capture_148/run_scan_proto2.py`.
