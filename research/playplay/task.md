**Estado:** Activo. Servicio HTTP validado — 2026-09-25.

- [x] Cerrar la [búsqueda directa en 148](docs/DIRECT_AES_SEARCH_148.md) con datos crudos y comprobación offline; negativo acotado, sin extractor directo validado.

- [x] Controlar procedencia/contaminación
- [x] Buscar huellas de claves de ronda
- [x] Obtener AES16 sin suministrarla al extractor mediante Side-Channel Attacks (DFA implementado con éxito)
- [x] Documentar el ataque DFA y los resultados.
- [x] Preservar capturadores recuperados con hash histórico y añadir controles de build/limpieza con pruebas offline.
- [x] Documentar procedencia de RVA de contextos y procedimiento condicionado para otras versiones.
- [x] Validar servicio Windows con versión/hash/firmas y conservar una corrida HTTP; el runner de captura anterior mantiene su alcance propio.
- [x] Evaluar Unicorn offline: generador y DFA positivos en dos contextos guardados; cadena desde licencia nueva pendiente.
- [x] Servicio HTTP AES16 y cliente cspot con `b4_seq` obligatorio; CLI compilada.
- [x] Validar extracción/contenido de dos recursos, reinicio ordenado y copia Windows por hashes.
- [ ] Probar reproducción completa de cspot y una licencia nueva fuera de los controles.

- [x] Conservar los hitos/experimentos del build148 y la procedencia de RVAs durante la depuración por versión.
- [x] Reconstruir ground truth token148/v5 con fuentes y verificación offline; actualizar consumidores activos.
