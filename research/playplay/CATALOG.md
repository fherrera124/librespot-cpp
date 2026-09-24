# Catálogo de recursos

Índice para elegir entradas sin leer todos los logs. Registro verificable:
[catalog.json](catalog.json). Los hechos actuales están en [STATUS](STATUS.md).

| Necesito… | ID del recurso | Entrada |
|---|---|---|
| Retomar el trabajo | current-state | [STATUS](STATUS.md), [PLAN](PLAN.md) |
| Entender qué falta | current-state | [Mecanismo](docs/MECANISMO_Y_EXTRACCION_AES.md) |
| Verificar árbol y evidencia | workspace-check | [Checker](tools/check_workspace.py) |
| Repetir resultado positivo Windows | token148-control | [Informe y comandos](docs/TOKEN148_ONESHOT_2026-09-24.md) |
| Solicitar licencia fresca | token148-request | [Probe local](tools/probe_token148_once.py) |
| Comprobar contenido sin Windows | token148-offline-verification | [Verificador](tools/verify_token148_content.py) |
| Encontrar fixtures conocidos | known-reference-inputs | [data](data/) |
| Revisar controles anteriores E | baseline-e | [Validación histórica](docs/VALIDATION_148_2026-09-24.md) |
| Capturar eventos nativos | native-tracing | [Catálogo detallado](tools/README.md) |
| Repetir captura limpia con control de build | clean-capture-148 | [Uso y procedencia](docs/CLEAN_CAPTURE_148.md) |
| Reconstruir contextos y portar RVAs | build-and-abi | [Secuencia y evidencia](docs/RVA_DISCOVERY_PLAYBOOK.md) |
| Evaluar DLL sin Spotify vivo | unicorn-feasibility-148 | [Resultados y límites](docs/UNICORN_FEASIBILITY_148.md) |
| Verificar binario y ABI | build-and-abi | [FACTS](docs/FACTS.md), [arquitectura](docs/PLAYPLAY_ARCHITECTURE_NOTES.md) |
| Reutilizar captura externa token | token-capture-external | [Grupo y límites](tools/token_capture_148/README.md) |
| Entender HTTP anterior | lan-prototypes | [Decisiones](docs/DECISIONS_148.md), fuera de alcance actual |
| Consultar antecedentes | research-history | [Índice documental](docs/README.md) |
| Encontrar copias anteriores | legacy-duplicates | [research/tools](../tools/README.md) |
| Ubicar análisis antiguo | legacy-toolbox | [Catálogo detallado](tools/README.md), inspeccionar paths |
| Repartir trabajo entre modelos | agent-workflow | [Flujo común](../../docs/AGENT_WORKFLOW.md) |

Los estados `content-verified` y `request-verified` describen solo ese control,
no madurez de producción ni extracción AES16. `historical` exige revisar contexto.

## Consultas rápidas

Desde la raíz del repo:

```sh
python3 research/playplay/tools/check_workspace.py --list
python3 research/playplay/tools/check_workspace.py --resource token148-control
python3 research/playplay/tools/check_workspace.py
```

El checker valida los documentos listados en catalog.json, no todos los enlaces
del repositorio. Omite fragments/anchors y URLs externas; no comprueba conectividad,
versión viva de Windows, corrección criptográfica ni carga de perfiles de modelos.
Comprueba existencia, tamaños y hashes de evidencia sin ejecutar scripts.

## Repetir el control de contenido offline

Python con cryptography; `--report` debe ser un archivo nuevo:

```sh
python3 research/playplay/tools/verify_token148_content.py \
  --vm research/playplay/docs/token148-content-vm-20260924T164814Z.json \
  --license research/playplay/docs/token148-license-20260924T164814Z-retry.json \
  --license research/playplay/docs/token148-license-second-20260924T164814Z.json \
  --report /tmp/token148-verificacion-nueva.json
```

Exit0 significa que pasan los casos incluidos. No usa red ni credenciales.
Un nuevo capturador de extracción debe mantener las referencias fuera de Windows;
el runner de control actual no cumple esa separación.

## Registrar recursos nuevos

Añadir un ID, estado, resumen, entorno, límites y paths relativos a raíz del repo
en catalog.json. Corridas con artefactos inmutables se registran en
[runs](runs/README.md). No copiar informes existentes para reorganizarlos:
los manifiestos permiten descubrirlos y verificar sus bytes en la ruta original.
