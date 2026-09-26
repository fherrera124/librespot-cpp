# Fuentes pendientes de estudio

Estos 25 archivos de código y parches estaban eliminados en la limpieza anterior.
Se recuperaron **sin modificar sus bytes** desde HEAD y se ordenaron bajo
`original-paths/` para conservar su ubicación de origen. El
[manifiesto](manifest.json) registra ruta, tamaño y SHA256 de cada uno.

**Estado: pendiente de revisión individual.** Su presencia no valida sus RVAs,
claves, token, dependencias, seguridad ni compatibilidad con Spotify 1.2.92.148.
No forman parte de los scripts operativos, del ground truth ni de los hitos
confirmados. Antes de usar uno, revisar su código, identificar build y
contratos, y contrastarlo con los controles actuales.

Los [39 scripts descartados](deletion-decisions.json) tienen una decisión por
ruta: versión ajena, cruce sin validación, copia redundante o prototipo ya
conservado como fuente de experimento. Por ejemplo, `find_cxx.py` referencia
el485; `find_hooks_148.py` ensayaba firmas del485 sin validar el hook148;
`trace_dump.py` dependía de constantes externas no verificadas. Las fuentes
de experimentos del148 conservadas en `runs/` siguen siendo evidencia primaria.
