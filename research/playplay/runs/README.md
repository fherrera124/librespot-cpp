# Corridas del build 1.2.92.148

Cada manifiesto registra rutas relativas a la raíz del repositorio, bytes y
SHA256. Se verifica con `python3 research/playplay/tools/check_workspace.py`.

- [Descubrimiento y diagnósticos148](20260924-discovery-148/manifest.json):
  evidencia de ABI, candidatos, contextos y primeros fallos DFA; no todos sus
  inputs constituyen controles criptográficos válidos.
- [Servicio HTTP AES16](20260925-http-dfa-service/manifest.json): dos licencias,
  recuperación DFA, contenido y despliegue.
- [Licencias token148](20260924T164814Z-token148/manifest.json): respuestas y
  prefijos cifrados usados como controles.
- [Capturas limpias](20260924-clean-capture-recovery/manifest.json): fuentes y
  contextos del mismo build.
- [Unicorn](20260924-unicorn-feasibility/manifest.json): DFA offline a partir de
  contextos de los dos recursos actuales.

Para corridas nuevas usar [la plantilla](../templates/EXPERIMENT.md), un ID único
y rutas `source/`, `raw/`, `evaluation/`, `manifest.json`. No sobrescribir
artefactos anteriores ni colocar credenciales en la evidencia.

La fuente original de un ensayo se conserva aunque tenga defectos documentados.
No adaptar sus bytes al nuevo fixture: actualizar el consumidor activo y mantener
la procedencia del ensayo. El manifiesto discovery148 sustituye la antigua
agrupación legacy-controls, conservando los hashes de los artefactos retenidos.
