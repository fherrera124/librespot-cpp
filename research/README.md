# Investigación PlayPlay del cliente vigente

El objetivo actual es Spotify Windows **1.2.92.148** con token148/v5. El estado,
la operación y las pruebas están en [PlayPlay](playplay/README.md). El único
binario conservado es [el dump 148](dlls/Spotify_1.2.92.148_dump.dll);
verificar siempre versión y hash del módulo vivo antes de usar RVAs.

El acceso Windows está descrito en [esta guía](acceso-ssh-pc-windows.md).
Para agentes: [reglas comunes](../AGENTS.md) y [reglas PlayPlay](playplay/AGENTS.md).

Entradas para reutilizar el trabajo del build:

- [Ground truth](playplay/data/README.md): dos licencias con AES, file_id,
  auxiliar b4, contenido cifrado y fuentes verificables.
- [Hito DFA](playplay/dfa_attack_results.md): recuperación y validación de claves.
- [Descubrimiento de RVAs](playplay/docs/RVA_DISCOVERY_PLAYBOOK.md): firmas,
  callers, contratos y evidencia de cómo se identificaron las direcciones.
- [Catálogo](playplay/CATALOG.md): herramientas, resultados positivos y negativos,
  manifiestos y próximos pasos.
- [Fuentes pendientes](playplay/deferred-review/README.md): scripts conservados
  para una evaluación individual posterior.

Se conserva la historia experimental de148 necesaria para reproducir los hitos.
La antigüedad de un ensayo no lo convierte en material de otro build.
