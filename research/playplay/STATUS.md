# Estado de Investigación: PlayPlay

**Estado:** ACTIVO. API HTTP de extracción validada (2026-09-25).

El servicio Windows devuelve AES16 mediante Frida/DFA desde `obfuscated_key`
y `b4_seq` obligatorios. Dos recursos, llamadas repetidas y prefijos de 4096 B
con Ogg/Vorbis y CRC válidos; también se comprobó con Spotify recién abierto y
tras parar/arrancar ordenadamente el servicio. Cliente cspot actualizado y CLI
compilada; reproducción completa y ESP32 pendientes. Servicio en loopback con
túnel SSH, tarea Windows a demanda. [Uso y límites](docs/HTTP_DFA_SERVICE.md),
[evidencia](runs/20260925-http-dfa-service/EXPERIMENT.md).

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

## Búsqueda directa cerrada

La [evaluación final](docs/DIRECT_AES_SEARCH_148.md) conserva datos crudos de dos
licencias y una comprobación offline: 2/2 bloques nativos válidos y 0
coincidencias K0..K10 completas o en mitades en las ventanas examinadas.
El negativo es acotado; no existe un extractor directo validado en 148.

## Pendientes para cuando se retome

1. Probar reproducción completa de cspot y una licencia nueva fuera de los dos controles.
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
