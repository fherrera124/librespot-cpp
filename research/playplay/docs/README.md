# Documentación PlayPlay 1.2.92.148

| Tema | Entrada |
|---|---|
| Servicio AES16 actual | [HTTP_DFA_SERVICE](HTTP_DFA_SERVICE.md) |
| Licencias y prefijos token148 | [TOKEN148_ONESHOT](TOKEN148_ONESHOT_2026-09-24.md) |
| Mecanismo y DFA | [MECANISMO_Y_EXTRACCION_AES](MECANISMO_Y_EXTRACCION_AES.md) |
| Resultado DFA del build | [Hito y evidencia](../dfa_attack_results.md) |
| Binario, hash y RVAs | [FACTS](FACTS.md), [arquitectura](PLAYPLAY_ARCHITECTURE_NOTES.md) |
| Búsqueda directa acotada | [DIRECT_AES_SEARCH](DIRECT_AES_SEARCH_148.md) |
| Captura y emulación | [CLEAN_CAPTURE](CLEAN_CAPTURE_148.md), [UNICORN](UNICORN_FEASIBILITY_148.md) |
| Ground truth vigente | [Esquema, campos y verificación](../data/README.md) |
| Pruebas de ABI y resultados negativos148 | [VALIDATION](VALIDATION_148_2026-09-24.md), [DECISIONS](DECISIONS_148.md) |
| Secuencia de hitos del build | [Resumen de trabajo](WINDOWS_WORK_SUMMARY.md), [PLAN](../PLAN.md) |
| Verificar/descubrir RVAs | [Playbook](RVA_DISCOVERY_PLAYBOOK.md) |

La evidencia se indexa en [runs](../runs/README.md). El estado de la
investigación está en [STATUS](../STATUS.md).

Los informes fechados conservan lo observado en cada etapa. Sus límites o tareas
pendientes se interpretan junto con STATUS; no invalidan hitos posteriores. La
[hipótesis AES256](history/2026-09-24-aes256-hypothesis.md) documenta una vía
posteriormente descartada por el control positivo AES128/token148.
