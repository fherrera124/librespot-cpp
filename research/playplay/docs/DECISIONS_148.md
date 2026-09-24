# Decisiones, descartes y condiciones para reabrir

Corte **2026-09-24**. Este registro conserva también resultados negativos.
«Descartado» se refiere al procedimiento o conclusión indicada, no a todas las
variantes posibles. [Evidencia y ensayos](VALIDATION_148_2026-09-24.md);
[plan vigente](../PLAN.md).

## Corregido o descartado con evidencia

| Procedimiento / afirmación anterior | Qué ocurrió | Decisión para continuar |
|---|---|---|
| Servidor «final» entrega AES y pasó 11 vectores | No se encontró evidencia conservada de ese éxito histórico; el ensayo propio terminó HTTP 500 | Es un prototipo no validado. Exigir informe de 22 resultados HTTP y control independiente de AES |
| Leer los primeros 16 bytes del resultado final | Son parte de una salida de 28 bytes cuyo prefijo cambia entre llamadas iguales; falla el control natural | No usarlos como AES. Conservar resultados iniciales como prueba del problema de extracción |
| La salida mide 24 bytes | Los primeros scripts leían 24; copia final de tamaño `0x1c` y capturas posteriores muestran 28 | Leer/copiar los 28; no completar el sufijo de informes antiguos por suposición |
| «system error» prueba token incompatible | Aparece con excepciones nativas capturadas por Frida; con `propagate` se ejecutan los vectores | Corregir el contrato de llamada antes de evaluar el resultado criptográfico |
| Reutilizar el mismo objeto VM | Historia de crash al mutar el objeto entre llamadas | Restaurar 144 bytes y copiar inicializador, o crear runtime nuevo; no guardar un puntero temporal como solución permanente |
| Trazar replays dentro del `onEnter` original | El primer tracer vio copias naturales pero ninguna en los replays | Diferir hasta el retorno natural, `traps: all`, ámbito por hilo; el tracer corregido sí las ve |
| Bloquear `onEnter` esperando peticiones | Los ensayos históricos congelaban la reproducción | No esperar en bucle dentro de un hook del hilo del cliente |
| Adjuntar a todos los procesos Spotify | Los auxiliares renderer/crashpad no son el proceso objetivo; historia de cuelgues | PID principal explícito y verificación de command line |
| Usar offsets del servidor remoto 667 en 148 | Servicio encontrado usaba `0x159330`/`0x3bd69c`; no son los offsets de esta investigación 148 | Se detuvo. No relanzar ni portar por nombre de versión; comprobar hash y firmas |
| Siempre hace falta otra canción | `0x4a0268` crea runtime fresco: 13 × 2 controles completados sin otro evento | Usar fixtures para ese ensayo. Reproducción Windows sigue necesaria para un **nuevo control natural** |
| Cambiar canción en Linux dispara necesariamente el hook Windows | El usuario aclaró la selección de dispositivo; Windows produjo la captura posterior | Confirmar reproducción local en Windows y el evento real; un comando Next aceptado no demuestra nueva licencia |
| Falta de evento del receptor de precarga prueba un error | El siguiente callback fue `0xd8dfb8`, ruta de reproducción, no `0x63fda4` | Captura principal válida; el receptor virtual de precarga sigue sin observarse |
| Código 2 del runner significa fallo de cualquier diagnóstico | Su evaluador solo reconoce `result` × 11 y control de candidato | Para los otros scripts leer `events`, `done`, `error` y sus controles; no modificar los JSON históricos |
| `Start-Process` basta para dejar el servidor vivo tras SSH | El proceso de ese intento desapareció al cerrar la conexión | Usar ejecución en primer plano con SSH vivo para experimentos; no se validó un servicio persistente |
| Fallo de firma del primer control de stream implica build cambiado | Había un byte mal transcrito en el script; corregido contra el dump, el control pasó | Mantener chequeos de firma y conservar ambos informes; no anularlos para «hacerlo andar» |

## Negativos que acotan, pero no resuelven la causa

| Resultado | Lo que sí permite decir | Lo que **no** permite decir / prueba que falta |
|---|---|---|
| Candidato RDX en `0x49f854`: control natural válido, 11 × 2 estables, 0/11 AES | Punto repetible, resultados distintos a los fixtures AES | No demuestra E/148 incompatible ni que RDX sea la AES final; hace falta control de contenido |
| Ninguna AES conocida entre fuentes de copias >=8 B | No apareció en los 13 lotes inspeccionados; búsqueda siguió tras el tope de muestras | No se exploraron todos los registros, accesos, offsets interiores ni memoria del proceso |
| Copias de 16 B solamente | Encontraron el candidato | No cubren la referencia 485: su hook lee 16 B desde una copia de tamaño 8 |
| Dos pruebas de caché sin Ogg/Vorbis | 102 y 116 prefijos legibles, un archivo bloqueado por corrida, ningún match bajo ese formato/IV | No hay asociación recurso→archivo ni formato de caché probado; no descarta AES |
| Bloques nativos distintos de AES(key, IV estándar) | No coincide esa fórmula con candidato natural ni AES conocida de vectores | Contador inicial/layout/salida aún desconocidos; no prueba que no sea CTR |
| AES inversa de cuatro bloques sin contador big-endian consecutivo | No coincide ese modelo para las claves ensayadas | No excluye otras representaciones, posiciones o transformaciones |
| Prefijo final variable pero stream repetible | La variación del descriptor no cambia los cuatro bloques de cada caso probado | No está revertido el formato ni demostrado el significado de `01000000` |
| Control de stream natural positivo | Snapshot de 740 bytes reproduce el bloque real | No identifica una AES en claro ni vincula ese contexto con un recurso de las capturas previas |
| Runtime fresco coincide con snapshot | Menor dependencia de haber capturado otra canción en el proceso ensayado | No prueba arranque frío, otra cuenta/build, funcionamiento offline o vida de fixtures tras reinicio |
| Timeouts de hooks | No hubo el evento esperado dentro del plazo | No prueban firma errónea, ruta imposible ni falta de claves: puede haber caché, otro dispositivo o rama |
| `b4_seq` no consumido en la parte del callback inspeccionada | Esos accesos observados no lo usan como AES | No se puede eliminar del contrato global; los vectores E no conservan ese campo |

## Vías históricas fuera del próximo paso

| Vía | Estado conservado | Cuándo tendría sentido reabrir |
|---|---|---|
| `LoadLibrary` standalone | El harness ensayado falló por entorno/dependencias; cargar el PE no reprodujo el proceso real | Hipótesis y control concretos de inicialización/ABI; no repetir el mismo harness como solución supuesta |
| Fórmula estática gen-B | No pasó los vectores de esta investigación | Nueva relación demostrable con el build/contrato objetivo |
| Unicorn con configs 483/485 sobre sub-builds locales | Hashes distintos y `UC_ERR_FETCH_UNMAPPED`; copia local del paquete tiene hash gate alterado | DLL canónico o config derivada y validada para el binario disponible; la emulación no queda descartada en general |
| Emular dump 148 | No hay resultado de éxito conservado | Registrar harness, relocaciones, SEH, ABI y control; no asumir que copiar RVAs de Frida resuelve el entorno |
| Actualizar a 667 | DLL extraído disponible, sin compatibilidad E ni RVAs validados aquí | Después de controlar extracción 148, o con evidencia independiente del par de 667 |
| MitM | Notas históricas de proxy; no se usó para los controles de esta sesión | Si hace falta conocer request/token/version; la captura interna actual solo observó respuestas |
| Parser C++ como causa del fallo criptográfico | El parser manual merece corrección, pero el VM falla antes de una AES validada | Corregir con un contrato HTTP probado y plan aprobado; no atribuirle los negativos internos |
| Nuevas appkeys/firmware partner | La investigación eSDK documenta controles positivo/negativo con el mismo harness y cuentas distintas; Sangean fue silencioso | Evidencia nueva que contradiga esos controles, no repetir búsquedas de credenciales como siguiente paso |

Las afirmaciones transitorias de otro agente («11 vectores pasaron», Hito 4
completo, parser ya corregido) fueron relecturas de estados luego revertidos,
no resultados propios. El C++ actual y los pendientes están en [FACTS.md](FACTS.md).
No se borraron informes negativos ni scripts históricos: se cambió su clasificación
y su recomendación de uso.
