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
Las fases siguientes conservan los hitos y dependencias de investigación del mismo build 148.

## Búsqueda directa en 148

[Cerrada con negativo acotado](docs/DIRECT_AES_SEARCH_148.md): dos licencias,
2/2 bloques nativos válidos y ninguna K0..K10 completa o en mitades en los
buffers examinados. DFA sigue siendo la vía operativa. No hay otro ensayo
directo programado.

## Meta principal y alcance logrado

Recuperar AES16 sin proporcionarla al cálculo de extracción: demostrado para
los dos controles token148/v5 mediante DFA. [Método y evidencia](dfa_attack_results.md).
La generalización a una licencia nueva y la reproducción completa siguen pendientes.

## Hito previo: procedencia y contratos del build 148

- [x] Identificar la rutina de copia `0x17780e0` mediante firma única.
- [x] Trazar la copia candidata `0x49f904` y la salida real de 28 B en `0x49f961`.
- [x] Validar replays del VM con snapshot restaurado y excepciones propagadas.
- [x] Capturar respuesta/licencia en `0x4a0494` y distinguir precarga/reproducción.
- [x] Identificar el constructor `0x4a0268` y el contrato de solicitud local256.
- [x] Seguir la ruta hasta init `0xd9e2e4` y stream `0xd9d0f0`; reproducir un bloque natural desde contexto740.
- [ ] Reconstruir el origen inicial no archivado de `0x49cb88`/`0x49eaa4`; no confundir confirmación con descubrimiento desde cero.

[Procedimiento, comandos y límites](docs/RVA_DISCOVERY_PLAYBOOK.md),
[ensayos de descubrimiento](docs/VALIDATION_148_2026-09-24.md).

## Fase 1: Limpieza de Entorno (Completada)
- [x] Construir un arnés (harness) Frida puro que pase `obfuscated_key` (16b) + `b4_seq` (4b) -> pipeline -> descriptor (28b) -> init -> context (740b) -> stream.
- [x] Separar referencias de captura: los capturadores limpios no insertan AES conocidas; ausencia de contaminación previa del proceso no demostrada.

## Fase 2: Búsqueda Criptográfica (Completada)
- [x] Volcar la memoria en los pasos intermedios.
- [x] Buscar claves de ronda en memoria plana (resultado: negativo).
- [x] Ejecutar un Differential Fault Analysis (DFA) sobre el bloque generador `0xd9d0f0`. (resultado: positivo).
- [x] Recuperar la Clave AES maestra resolviendo matemáticamente las diferencias generadas por el fallo.

## Fase 3: Automatización e integración
- [ ] Ejecutar una captura nueva con el runner protegido y archivar fuentes/informe; la validación offline no sustituye este control Windows.
- [x] Implementar el solver DFA y la inversión de K10 en `extractor.py`/`reverse_key_schedule.py`.
- [x] Integrar el cálculo en el servicio con preflight, b4_seq obligatorio, verificación AES, worker aislado y cierre ordenado.
- [x] Mantener un ground truth token148/v5 verificable por hashes, bloques y CRC.
- [ ] Probar licencia fresca y pista completa; las pruebas con fixtures no sustituyen este control.

## Alternativa evaluada: Unicorn sin proceso vivo

Generador/DFA funcionan con snapshots de dos recursos. Para prescindir también
de esas capturas falta construir el contexto desde la entrada ofuscada dentro
del emulador, corrigiendo ABI, mapeo e imports/runtime. Ver
[viabilidad y criterios de cierre](docs/UNICORN_FEASIBILITY_148.md). Esta evaluación
no demuestra todavía extracción autónoma de una licencia nueva.
