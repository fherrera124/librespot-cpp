# Reglas de trabajo PlayPlay

Aplican también las [reglas del repositorio](../../AGENTS.md).

## Contexto mínimo

Leer [README](README.md) → [STATUS](STATUS.md) → [PLAN](PLAN.md) y [task](task.md).
Usar [CATALOG](CATALOG.md) para encontrar entradas concretas. No leer todas las
bitácoras o informes para orientarse. Antes de instrumentación/ABI, leer también
[arquitectura](docs/PLAYPLAY_ARCHITECTURE_NOTES.md) y [FACTS](docs/FACTS.md).
Para portar RVAs o reconstruir contextos, seguir el
[playbook de descubrimiento](docs/RVA_DISCOVERY_PLAYBOOK.md): distinguir evidencia
histórica, candidatos y contratos validados; no trasladar constantes entre builds.

Autoridad: instrucciones del usuario → estas reglas → evidencia primaria para
los hechos → STATUS (síntesis actual) → PLAN (siguiente paso) → task (checklist).
Los informes de corridas describen su experimento; STATUS resume el estado vigente.
Ante discrepancia, señalarla y contrastar evidencia, no elegir por fecha o autor.

## Ejecución

- Inspeccionar el código antes de ejecutar un script, incluso si se llama final.
- Preflight: verificar proceso principal, build y hash antes de usar RVAs. PID,
  base ASLR y disponibilidad remota son efímeros.
- SSH con la clave de [acceso documentado](../acceso-ssh-pc-windows.md).
- Un único operador Windows/Frida. Identificar procesos propios y cerrar solo
  los del ensayo; no matar todas las instancias del usuario para limpiar.
- Alcance actual autorizado: API local Windows de AES16 consumida por cspot
  mediante túnel SSH; ver [operación](docs/HTTP_DFA_SERVICE.md). Credenciales
  de cuenta locales; a Windows solo la licencia, nunca bearer/Client-Token de cspot.
- Captura de extracción y verificación van separadas: mantener AES de referencia
  fuera de Spotify y no usar coincidencias contaminadas como clave extraída.
- Para integración C++, presentar el hallazgo y plan antes de editar y respetar
  la autorización ya dada en la conversación; la reorganización no autoriza
  cambios de reproducción. No publicar endpoints ni reabrir túneles públicos.

## Archivos y cierre

- Código reutilizable en tools; scripts para un experimento en runs/<id>/source.
  Entradas verificadas en data; documentación en docs.
- La limpieza se decide por build y evidencia, no por fecha ni por resultado
  positivo. Conservar hitos, métodos de descubrimiento, fuentes y datos primarios
  del build148. Distinguir diagnósticos/candidatos de los controles token148/v5
  validados en data/ground-truth-vectors.json; no resumir suprimiendo procedencia.
- Corridas nuevas según [runs/README](runs/README.md). No sobrescribir los
  artefactos conservados ni alterar sus hashes.
- Añadir recursos al catálogo antes de pedir a otro agente que los reutilice.
  No copiar scripts históricos para crear otra versión sin identificar procedencia.
- Actualizar el checklist y el estado solo con alcance demostrado. Una estimación no es un test.
- Delegación según [flujo común](../../docs/AGENT_WORKFLOW.md): contexto mínimo,
  tarea concreta, único escritor y evidencia revisada por el coordinador.
