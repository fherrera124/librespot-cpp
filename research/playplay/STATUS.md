# Estado de Investigación: PlayPlay

**Progreso Actual:** FASE DE RESOLUCIÓN

Se ha resuelto el núcleo criptográfico del White-Box de Arxan (PlayPlay v1.2.92.148). La clave maestra AES-128 ha sido extraída exitosamente sin depender de inyecciones conocidas previas y sin que la clave exista en memoria plana.

## Hitos Completados
- [x] Ejecución de la cadena de licenciamiento en VM Windows mediante Frida sin contaminación de claves AES planas en el cliente.
- [x] Volcado y análisis estructural del `context` de 740 bytes de inicialización (`0xd9e2e4`).
- [x] Confirmación de la ausencia de huellas directas de las claves de ronda en memoria plana.
- [x] **Inyección de fallos (DFA)** en las claves de ronda cacheadas en el `context`.
- [x] **Recuperación matemática completa** de la Clave AES Maestra invirtiendo el *Key Schedule* desde la Clave de la Ronda 10 extraída.

## Estado de la Tarea Actual
* Completado: Extracción de AES16 puramente mediante métodos de criptoanálisis de caja blanca (DFA) sobre el bloque generador.

## Siguientes Pasos
1. Consolidar el pipeline del ataque DFA en un único script de extracción de producción.
2. Comprobar otros binarios si fuese necesario.
