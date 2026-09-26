# Catálogo del build 1.2.92.148

El registro verificable es [catalog.json](catalog.json). Entradas principales:

| Necesidad | Recurso |
|---|---|
| Estado y próximos pasos | [STATUS](STATUS.md), [PLAN](PLAN.md) |
| Operar la API AES16 | [HTTP_DFA_SERVICE](docs/HTTP_DFA_SERVICE.md) |
| Comprobar dos licencias y contenido | [Corrida HTTP](runs/20260925-http-dfa-service/EXPERIMENT.md) |
| Ground truth actual: AES, licencias y procedencia | [Datos y esquema](data/README.md), [validador](tools/check_ground_truth_148.py) |
| Revisar el hito DFA | [Resultados](dfa_attack_results.md) |
| Revisar la búsqueda directa | [Resultado y datos](docs/DIRECT_AES_SEARCH_148.md) |
| Probar emulación desde contextos | [Unicorn](docs/UNICORN_FEASIBILITY_148.md) |
| Capturar contextos del build | [Captura limpia](docs/CLEAN_CAPTURE_148.md) |
| Reconstruir los ensayos que descubrieron el pipeline | [Validación148](docs/VALIDATION_148_2026-09-24.md), [decisiones](docs/DECISIONS_148.md), [manifiesto](runs/20260924-discovery-148/manifest.json) |
| Ejecutar utilitarios y trazadores | [tools](tools/README.md) |
| Estudiar scripts aún no evaluados | [Fuentes pendientes](deferred-review/README.md) y [manifiesto](deferred-review/manifest.json) |
| Consultar RVAs y ABI | [FACTS](docs/FACTS.md), [playbook](docs/RVA_DISCOVERY_PLAYBOOK.md) |

```sh
python3 research/playplay/tools/check_workspace.py --list
python3 research/playplay/tools/check_workspace.py
python3 research/playplay/tools/check_ground_truth_148.py
python3 research/playplay/tools/check_direct_aes_148.py
```

El checker comprueba rutas, enlaces y hashes de evidencia; no contacta Windows.
Una licencia nueva y reproducción completa siguen pendientes.

Se preservan también los resultados negativos del mismo build cuando explican
el descubrimiento o descartan una hipótesis. Los datos originales mantienen sus
hashes; el ground truth activo se limita a los dos controles validados. Fuentes
incompletas conservadas en runs/source documentan un experimento, no una herramienta
aprobada. Los materiales exclusivos de otros builds quedan fuera del árbol activo.
