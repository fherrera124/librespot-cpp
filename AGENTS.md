# Instrucciones comunes para agentes

## Orientarse antes de trabajar

Este repositorio contiene cspot/librespot-cpp (C++), CLI Linux, ESP32 y material
de investigación. Responder al usuario en español. Leer solo la rama relevante:

| Trabajo | Entrada |
|---|---|
| Biblioteca, builds y arquitectura general | [README](README.md), [main](main/README.md) |
| CLI Linux | [targets/cli](targets/cli/README.md) |
| ESP32 | [targets/esp32](targets/esp32/README.md) |
| PlayPlay / claves de audio | [research/playplay](research/playplay/README.md) y su [AGENTS](research/playplay/AGENTS.md) |
| Investigación anterior eSDK | [research](research/README.md) |
| Delegación, modelos y handoff | [Flujo de agentes](docs/AGENT_WORKFLOW.md) |

Las instrucciones del usuario prevalecen sobre estos documentos. No inferir
que una nota antigua amplía la tarea actual. Un resultado debe enlazar evidencia;
una hipótesis o estimación debe estar etiquetada como tal.

## Conservar el trabajo compartido

- Al empezar, consultar `git status --short` y el diff del área asignada.
- No resetear, limpiar, stagear ni commitear cambios ajenos. El índice y los
  submódulos pueden contener trabajo de otras sesiones; verificar, no asumir.
- Un escritor por archivo y un operador por entorno remoto. La asignación y
  los archivos reservados se comunican antes de delegar.
- No leer ni incluir valores de session.json, credenciales o claves SSH salvo
  que el flujo autorizado los necesite; no incorporarlos a informes ni commits.
- Inspeccionar scripts de investigación antes de ejecutarlos. No recorrer
  ppvenv, binarios, builds o logs completos para una pregunta documental.
- Ejecutar comprobaciones proporcionales. Documentación: enlaces/diff; cambios
  de código: pruebas de comportamiento relevantes. No repetir pruebas ya válidas
  sin cambios, fallos o una hipótesis nueva.

## Uso de agentes y modelos

El usuario solicita colaboración eficiente entre modelos. Delegar subtareas
acotadas e independientes cuando permitan avanzar en paralelo con trabajo útil
del coordinador; resolver localmente las tareas pequeñas. Usar los perfiles de
[AGENT_WORKFLOW](docs/AGENT_WORKFLOW.md), sujetos a modelos/herramientas disponibles.
No crear equipos por defecto ni copiar toda la conversación a cada agente.

Los perfiles [`.codex/agents`](.codex/agents/) son configuración local de roles,
no garantía de que cualquier cliente los cargue. Si la herramienta permite elegir
modelo, hacerlo explícitamente; si no, registrar herencia. No afirmar cambios
de modelo o ahorros que no se hayan observado.
