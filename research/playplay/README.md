# PlayPlay — empezar aquí

Investigación de interoperabilidad de cspot con **Spotify 1.2.92.148 y token148/v5**. El estado vigente está en
[STATUS.md](STATUS.md); las instrucciones comunes en [AGENTS.md](AGENTS.md).

## Retomar con poco contexto

1. Leer [STATUS](STATUS.md): resultado demostrado, límites y siguiente trabajo.
2. Leer [PLAN](PLAN.md) y [task](task.md): decisión y checklist actuales.
3. Elegir un recurso en [CATALOG](CATALOG.md); inspeccionar su código antes de usarlo.
4. Abrir arquitectura, informes o historia solo según la tarea.

No empezar por todos los logs. Para una explicación introductoria:
[mecanismo y extracción AES](docs/MECANISMO_Y_EXTRACCION_AES.md).

## Mapa del árbol

```text
playplay/
  AGENTS.md, GEMINI.md       reglas y entrada compatible
  STATUS.md                 hechos actuales y límites
  PLAN.md, task.md           siguiente paso y progreso
  CATALOG.md, catalog.json   búsqueda de recursos reutilizables
  tools/                    herramientas; token_capture_148 agrupa captura externa
  data/                     fixtures y prefijos cifrados conservados
  runs/                     manifiestos de evidencia y corridas nuevas
  deferred-review/          fuentes sin evaluación individual para estudio posterior
  docs/                     explicaciones, contratos e informes narrativos
    external/               documentos aportados por otros agentes
    history/                argumentos y checklists superados
  ppvenv/                   entorno local, no fuente de información del proyecto
```

Los JSON/logs existentes en docs conservan sus rutas y bytes para no alterar
la evidencia ni comandos históricos. Sus hashes están indexados en runs.
Los nuevos ensayos agrupan artefactos según [runs/README](runs/README.md).
DLLs compartidos en [../dlls](../dlls/), fuera de esta carpeta.
Las [fuentes pendientes](deferred-review/README.md) conservan su ruta y hash
originales; no son herramientas operativas del build148.

## Ground truth e hitos del build actual

[data/ground-truth-vectors.json](data/ground-truth-vectors.json) reúne los dos
casos token148/v5 comprobados: file_id, entrada ofuscada, b4_seq, AES, K10,
bloque nativo, IV, prefijos cifrados y procedencia. Es el fixture para nuevas
comprobaciones; los informes de descubrimiento anteriores conservan sus bytes
y sus límites, incluso cuando sus candidatos no resultaron ser AES.

El [hito DFA](dfa_attack_results.md) conserva el método de recuperación;
[el playbook](docs/RVA_DISCOVERY_PLAYBOOK.md) explica cómo se llegó a cada RVA,
las recetas de análisis y los huecos de procedencia. La [API HTTP](docs/HTTP_DFA_SERVICE.md)
implementa el flujo actual y la [evaluación Unicorn](docs/UNICORN_FEASIBILITY_148.md)
conserva la vía de emulación desde snapshots.

## Encontrar y verificar

Desde cualquier directorio, pasando la ruta del script:

```sh
python3 research/playplay/tools/check_workspace.py --list
python3 research/playplay/tools/check_workspace.py --resource ground-truth-148
python3 research/playplay/tools/check_workspace.py
```

Los ejemplos suponen cwd en la raíz del repo. El checker resuelve sus recursos
respecto del propio archivo, no requiere red ni credenciales ni inicia Spotify.

Para detalles de uso: [catálogo humano](CATALOG.md), [catálogo de scripts](tools/README.md),
[índice documental](docs/README.md), [trabajo entre modelos](../../docs/AGENT_WORKFLOW.md).
