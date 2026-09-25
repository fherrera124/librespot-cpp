# Plan de Investigación: PlayPlay

**Estado:** Activo. Servicio HTTP validado — 2026-09-25.

## Integración HTTP actual

- [x] API Windows `POST /deob` que exige licencia completa y devuelve AES16 verificada.
- [x] Worker aislado, límite de tiempo, exclusión de solicitudes y arranque exclusivo.
- [x] Cliente C++ conserva `b4_seq`, valida JSON/AES y configura URL/token.
- [x] Pruebas con dos recursos por HTTP y CRC de contenido; parada/arranque ordenados.
- [x] CLI compilada y copia Windows comparada por hash con el repositorio.
- [ ] Reproducción completa en cspot y licencia nueva fuera de los controles.
- [ ] Validación ESP32 y de otras versiones si se requieren.

[Operación](docs/HTTP_DFA_SERVICE.md), [corrida](runs/20260925-http-dfa-service/EXPERIMENT.md).
Las fases siguientes conservan el historial anterior a esta integración.

## Búsqueda directa en 148

[Cerrada con negativo acotado](docs/DIRECT_AES_SEARCH_148.md): dos licencias,
2/2 bloques nativos válidos y ninguna K0..K10 completa o en mitades en los
buffers examinados. DFA sigue siendo la vía operativa. No hay otro ensayo
directo programado.

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
