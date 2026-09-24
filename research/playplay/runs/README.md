# Corridas y manifiestos de evidencia

Cada manifiesto registra rutas **relativas a la raíz del repositorio**, tamaño
y SHA256. Se verifica con `python3 research/playplay/tools/check_workspace.py`.

- [token148 positivo](20260924T164814Z-token148/manifest.json): evidencia de la prueba puntual.
- [Controles previos](20260924-legacy-controls/manifest.json): fixtures e informes anteriores.

Los artefactos anteriores a esta organización mantienen sus rutas docs/data y
sus bytes. Los manifiestos los indexan sin duplicarlos ni reescribir campos
internos. Un path de un informe antiguo puede referirse al entorno original.

## Para corridas nuevas

```text
runs/<YYYYMMDDTHHMMSSZ>-<tema>/
  EXPERIMENT.md   hipótesis, propietario, preflight y comandos sin secretos
  source/        copia exacta de los scripts ejecutados
  inputs/        licencia/fixtures sin credenciales de cuenta
  raw/           resultados y logs originales
  evaluation/    evaluación independiente y sus límites
  manifest.json  rutas, bytes y SHA256 de los artefactos al cerrar
```

Usar [plantilla](../templates/EXPERIMENT.md). IDs únicos, sin sobrescribir ensayos.
Mantener las AES de referencia fuera de Windows en capturas de extracción.
Guardar modelos efectivos como metadatos, no atribuir una conclusión al modelo.

El formato `schema_version:1` del manifiesto contiene `run_id`, `status`,
`summary` y `artifacts`; cada artefacto tiene `path`, `sha256`, `bytes`, `role`.
Agregar el manifiesto a `catalog.json.evidence_manifests`. No recalcular hashes
para ocultar un cambio accidental: diagnosticarlo contra la evidencia original.

Rutas de scripts reubicados en [registro de movimientos](tree-migration-20260924.json).
