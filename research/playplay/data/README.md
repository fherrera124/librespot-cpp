# Entradas y ground truth del build 1.2.92.148

[ground-truth-vectors.json](ground-truth-vectors.json), **schema_version 2**,
reúne los dos casos token148/v5 que tienen AES validada. Sustituye el antiguo
schema de vectores E: no es compatible con consumidores que esperen
`vectors_token_E`. Los consumidores activos se actualizan explícitamente.

| Campo | Significado |
|---|---|
| `build` | Versión 1.2.92.148 y SHA256 de la DLL en disco |
| `token.hex`, `token.version` | Constante PlayPlay del request y protocolo5; no credencial de cuenta |
| `cases[].file_id` | Identificador de20 B del archivo de audio |
| `obfuscated_key`, `b4_seq` | Campos de16 B y4 B de la respuesta de licencia |
| `aes`, `k10` | AES128 de contenido validada y clave de ronda10 recuperada |
| `native_block16`, `aes_ctr.iv` | Primer bloque observado y contador/IV usado para contrastarlo |
| `content` | Prefijo cifrado de4096 B, ruta y hash |
| `provenance` | Fuentes de licencia, DFA, contexto y captura directa, enlazadas por hashes |

```sh
python3 research/playplay/tools/check_ground_truth_148.py
```

El checker no contacta servicios ni lee credenciales. Dos pares válidos no
prueban soporte de cualquier licencia: una tercera licencia fresca sigue pendiente.

Los demás JSON de esta carpeta son **fixtures de experimentos sobre148**:
`cache-probe-*`, `key-pipeline-controls-*` y `wrapped-key-controls-*` contienen
candidatos/entradas usadas para descubrir ABI, probar repetibilidad o rechazar
hipótesis. El campo `aes` de una fila con `aes_known=false` era un candidato,
no una AES validada. Los fixtures `token148-{fresh,content}-controls-*` contienen
además un control natural auxiliar y variantes con b4 a cero; conservan el
formato y hash exactos de sus corridas. No incorporarlos automáticamente al
nuevo ground truth. La [validación148](../docs/VALIDATION_148_2026-09-24.md) y el
[control de contenido](../docs/TOKEN148_ONESHOT_2026-09-24.md) explican sus alcances.
