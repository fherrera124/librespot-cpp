# Trabajo entre agentes y elección de modelos

## Roles y esfuerzo

Asignación de proyecto, revisada el 2026-09-24. Verificar disponibilidad en cada
sesión; no implica precios, cuotas ni disponibilidad universal.

| Rol | Modelo preferido | Esfuerzo inicial | Trabajo apropiado |
|---|---|---|---|
| Inventario | gpt-6-luna | medium | Rutas, duplicados, enlaces y resúmenes acotados; solo lectura |
| Implementación | gpt-6-sol | medium | Cambios delimitados, scripts, pruebas e integración |
| Revisión difícil | gpt-6-astra | high | Inferencias criptográficas, ABI, contradicciones o decisiones difíciles; solo lectura salvo asignación explícita |
| Coordinación | Modelo de la sesión principal | El de la sesión | Divide tareas, asigna recursos exclusivos, valida e integra |

Los nombres son los disponibles al configurar este workspace. Preferir un rol
equivalente disponible antes que bloquear una tarea por una marca/modelo ausente.
Para una revisión ordinaria, Sol puede ser suficiente; no escalar automáticamente.
Perfiles en [.codex/agents](../.codex/agents/), sin cambios a configuración global.

OpenAI Docs permite elegir modelo/esfuerzo por agente y advierte que cada agente
añade consumo de tokens. Por eso esta política reserva la delegación para tareas
independientes y usa contexto reducido. Los roles concretos de la tabla son una
decisión de este proyecto, no una medición comparativa.
[Documentación oficial](https://learn.chatgpt.com/docs/agent-configuration/subagents).

## Asignación breve

El coordinador entrega este contrato en el mensaje de tarea; no es obligatorio
crear un archivo para cada consulta pequeña:

```text
Objetivo y criterio de cierre:
Rol / modelo efectivo / esfuerzo:
Entradas obligatorias: AGENTS aplicable, STATUS, PLAN y archivos concretos.
Alcance: solo lectura o archivos que este agente puede editar.
Recursos reservados: archivos / Windows / PID / ninguno.
Salida: hallazgos con rutas, parche o informe; formato y límite de extensión.
Límites: sin SSH / sin credenciales / sin experimentos / otros según tarea.
Escalar cuando: evidencia contradictoria o límite de tarea alcanzado.
```

No delegar al agente una lectura ilimitada del repositorio. Para PlayPlay entregar
IDs del [catálogo](../research/playplay/CATALOG.md) y el siguiente experimento.
Consultar el catálogo con Python antes de cargar informes de megabytes.

## Coordinación y ahorro

1. Dividir por resultados independientes; evitar dos agentes investigando lo mismo.
2. Un escritor por archivo. El coordinador mantiene STATUS, PLAN y task; delegados
   devuelven hallazgos y evidencia. No usar esos archivos como lock automático.
3. Solo un agente opera Windows/Frida. Paralelizar análisis offline y documentación,
   no hooks simultáneos sobre un proceso compartido.
4. Usar contexto nuevo y entradas mínimas; ampliar solo ante una dependencia real.
5. Pedir un resultado corto con rutas, pruebas, límites y pendiente concreto.
6. Revisar antes de integrar. Coincidencias o mayorías entre modelos no son evidencia.
7. Elevar dificultad si falta razonamiento, no repetir el mismo intento con todos
   los modelos. No delegar recursivamente salvo asignación expresa.

Límite recomendado: coordinador más uno o dos colaboradores. Respetar siempre
el límite real del entorno. Este documento no cambia las herramientas disponibles.

## Handoff de investigación

Un experimento nuevo usa `research/playplay/runs/<UTC>-<tema>/`; seguir el
[formato de corridas](../research/playplay/runs/README.md). Guardar fuentes exactas
ejecutadas, entrada sin credenciales, informe y evaluación separados.

Al cerrar: actualizar STATUS si cambian hechos, PLAN si cambia el siguiente paso,
task para progreso y la bitácora para el registro. Incluir procesos propios
restantes y recursos liberados. No borrar evidencia negativa ni reescribir JSON
anteriores. Pasar [check_workspace.py](../research/playplay/tools/check_workspace.py).
