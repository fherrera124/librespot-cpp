# Reglas de trabajo PlayPlay

Aplican también las [reglas del repositorio](../../AGENTS.md).

## Contexto mínimo

Leer [README](README.md) → [STATUS](STATUS.md) → [PLAN](PLAN.md) y [task](task.md).
Usar [CATALOG](CATALOG.md) para encontrar entradas concretas. No leer todas las
bitácoras o informes para orientarse. Antes de instrumentación/ABI, leer también
[arquitectura](docs/PLAYPLAY_ARCHITECTURE_NOTES.md), [FACTS](docs/FACTS.md) y las
[decisiones](docs/DECISIONS_148.md) pertinentes.
Para portar RVAs o reconstruir contextos, seguir el
[playbook de descubrimiento](docs/RVA_DISCOVERY_PLAYBOOK.md): distinguir evidencia
histórica, candidatos y contratos validados; no trasladar constantes entre builds.

Autoridad: instrucciones del usuario → estas reglas → evidencia primaria para
los hechos → STATUS (síntesis actual) → PLAN (siguiente paso) → task (checklist).
Los documentos históricos describen su experimento; no son el estado vigente.
Ante discrepancia, señalarla y contrastar evidencia, no elegir por fecha o autor.

## Ejecución

- Inspeccionar el código antes de ejecutar un script, incluso si se llama final.
- Preflight: verificar proceso principal, build y hash antes de usar RVAs. PID,
  base ASLR y disponibilidad remota son efímeros.
- SSH con la clave de [acceso documentado](../acceso-ssh-pc-windows.md).
  No ejecutar scripts desde tools/archive como si todos fueran vigentes.
- Un único operador Windows/Frida. Identificar procesos propios y cerrar solo
  los del ensayo; no matar todas las instancias del usuario para limpiar.
- Alcance actual autorizado: API local Windows de AES16 consumida por cspot
  mediante túnel SSH; ver [operación](docs/HTTP_DFA_SERVICE.md). Credenciales
  de cuenta locales; a Windows solo la licencia, nunca bearer/Client-Token de cspot.
- Captura de extracción y verificación van separadas: el runner histórico inyecta
  AES conocidas; no usar sus coincidencias de memoria como clave extraída.
- Para integración C++, presentar el hallazgo y plan antes de editar y respetar
  la autorización ya dada en la conversación; la reorganización no autoriza
  cambios de reproducción. No publicar endpoints ni reabrir túneles públicos.

## Archivos y cierre

- Código reutilizable en tools; scripts para un experimento en runs/<id>/source.
  Entradas verificadas en data; documentación en docs; antecedentes en archive.
- Corridas nuevas según [runs/README](runs/README.md). Informes existentes
  permanecen en sus rutas, registrados por hashes. No sobrescribirlos.
- Añadir recursos al catálogo antes de pedir a otro agente que los reutilice.
  No copiar scripts históricos para crear otra versión sin identificar procedencia.
- Registrar avances y bloqueos en [bitácora](docs/WINDOWS_WORK_SUMMARY.md), actualizar
  el checklist y el estado solo con alcance demostrado. Una estimación no es un test.
- Delegación según [flujo común](../../docs/AGENT_WORKFLOW.md): contexto mínimo,
  tarea concreta, único escritor y evidencia revisada por el coordinador.
