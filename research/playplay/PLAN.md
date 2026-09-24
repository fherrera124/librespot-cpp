# Plan de Investigación: PlayPlay

**Pausado por indicación del usuario — 2026-09-24.** Las tareas pendientes
quedan como referencia para retomar; no hay nuevos experimentos en ejecución.

## Meta Principal (Lograda)
Obtener la AES de 16 bytes a partir del ejecutable de Spotify sin proporcionarla como entrada, quebrando efectivamente el DRM White-Box de PlayPlay.

## Fase 1: Limpieza de Entorno (Completada)
- [x] Construir un arnés (harness) Frida puro que pase `obfuscated_key` (16b) + `b4_seq` (4b) -> pipeline -> descriptor (28b) -> init -> context (740b) -> stream.
- [x] Auditar que no exista la clave AES pre-cargada.

## Fase 2: Búsqueda Criptográfica (Completada)
- [x] Volcar la memoria en los pasos intermedios.
- [x] Buscar claves de ronda en memoria plana (resultado: negativo).
- [x] Ejecutar un Differential Fault Analysis (DFA) sobre el bloque generador `0xd9d0f0`. (resultado: positivo).
- [x] Recuperar la Clave AES maestra resolviendo matemáticamente las diferencias generadas por el fallo.

## Fase 3: Industrialización (Próximo)
- [ ] Ejecutar una captura nueva con el runner protegido y archivar fuentes/informe; la validación offline no sustituye este control Windows.
- [ ] Desarrollar una herramienta `extractor.py` que una el proceso automático:
  1. Conexión Frida a Spotify PID.
  2. Inyección de `obfuscated_key` y ejecución del pipeline.
  3. Ejecución del bloque de audio correcto y 4 versiones con fallos.
  4. Solución DFA offline e inversión del Key Schedule.
  5. Devolución de la Clave AES en texto plano.

## Alternativa evaluada: Unicorn sin proceso vivo

Generador/DFA funcionan con snapshots de dos recursos. Para prescindir también
de esas capturas falta construir el contexto desde la entrada ofuscada dentro
del emulador, corrigiendo ABI, mapeo e imports/runtime. Ver
[viabilidad y criterios de cierre](docs/UNICORN_FEASIBILITY_148.md). Esta evaluación
no demuestra todavía extracción autónoma de una licencia nueva.
