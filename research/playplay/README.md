# PlayPlay — empezar aquí

Investigación de interoperabilidad de cspot. El estado vigente está en
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
  docs/                     explicaciones, contratos e informes narrativos
    external/               documentos aportados por otros agentes
    history/                argumentos y checklists superados
  resources/, archive/      prototipos y pruebas antiguas; no asumir vigencia
  versions/                 material de builds alternativos
  ppvenv/                   entorno local, no fuente de información del proyecto
```

Los JSON/logs existentes en docs conservan sus rutas y bytes para no alterar
la evidencia ni comandos históricos. Sus hashes están indexados en runs.
Los nuevos ensayos agrupan artefactos según [runs/README](runs/README.md).
DLLs compartidos en [../dlls](../dlls/), fuera de esta carpeta.

## Encontrar y verificar

Desde cualquier directorio, pasando la ruta del script:

```sh
python3 research/playplay/tools/check_workspace.py --list
python3 research/playplay/tools/check_workspace.py --resource token148-control
python3 research/playplay/tools/check_workspace.py
```

Los ejemplos suponen cwd en la raíz del repo. El checker resuelve sus recursos
respecto del propio archivo, no requiere red ni credenciales ni inicia Spotify.

Para detalles de uso: [catálogo humano](CATALOG.md), [catálogo de scripts](tools/README.md),
[índice documental](docs/README.md), [trabajo entre modelos](../../docs/AGENT_WORKFLOW.md).
