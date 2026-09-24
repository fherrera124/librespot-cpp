# Estado de Investigación: PlayPlay

**Estado:** PAUSADO por indicación del usuario (2026-09-24). Conservar los
resultados y pendientes; no iniciar nuevos experimentos hasta que se retome.

Se reprodujo la recuperación AES-128 mediante DFA para los casos documentados
del build 1.2.92.148, sin proporcionar la AES al cálculo de extracción. Esto no
demuestra ausencia de la clave en toda la memoria ni independencia completa
de un proceso previo: los ensayos Unicorn parten de contextos capturados.

## Hitos Completados
- [x] Capturas Windows mediante scripts que no insertan AES de referencia; ausencia de contaminación previa del proceso no demostrada.
- [x] Volcado y análisis estructural del `context` de 740 bytes de inicialización (`0xd9e2e4`).
- [x] Búsqueda de huellas de claves de ronda con resultado negativo en las regiones examinadas; no prueba de ausencia global.
- [x] **Inyección de fallos (DFA)** en las claves de ronda cacheadas en el `context`.
- [x] **Recuperación matemática completa** de la Clave AES Maestra invirtiendo el *Key Schedule* desde la Clave de la Ronda 10 extraída.

## Estado de la Tarea Actual
* Completado: Extracción de AES16 puramente mediante métodos de criptoanálisis de caja blanca (DFA) sobre el bloque generador.

## Pendientes para cuando se retome
1. Consolidar el pipeline del ataque DFA en un único script de extracción de producción.
2. Comprobar otros binarios si fuese necesario.

## Mantenimiento de capturas y portabilidad

Los dos capturadores recuperados se preservaron con los hashes de sus informes
históricos. Las herramientas actuales añaden control de versión/hash/firmas y
limpieza ante errores, comprobados offline; falta una corrida Windows de esta
revisión. Ver [uso y límites](docs/CLEAN_CAPTURE_148.md) y
[procedencia de RVA y contextos](docs/RVA_DISCOVERY_PLAYBOOK.md).

## Evaluación Unicorn sin proceso vivo

El generador se ejecutó en Unicorn desde snapshots de dos recursos; con 16 fallos
por recurso se recuperaron ambas AES offline y se validaron prefijos4096 con CRC.
No se accedió a Windows. La construcción desde clave ofuscada/descriptor sin
snapshot sigue sin funcionar: ABI del prototipo incorrecta y entorno/imports sin
resolver. [Evidencia y siguiente trabajo](docs/UNICORN_FEASIBILITY_148.md).
