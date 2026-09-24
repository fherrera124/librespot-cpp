# Bitácora de Windows — sesiones 2026-09-24

Para retomar leer [STATUS](../STATUS.md) y [PLAN](../PLAN.md). Esta bitácora
conserva etapas sucesivas; los PID y próximos pasos antiguos no son vigentes.

## Resumen de las etapas

**Actualización posterior — token148/v5:** el [ensayo puntual por SSH](TOKEN148_ONESHOT_2026-09-24.md)
validó dos licencias y dos prefijos CDN contra el generador nativo: 4096 bytes
AES-128-CTR idénticos por recurso, dos variantes b4_seq, dos ejecuciones;
Ogg/Vorbis con CRC válido. No se inició servicio ni se modificó C++.
El candidato sigue sin ser la AES. Los apartados anteriores al cierre nuevo
conservan la historia, no sustituyen estos resultados.

Se ejecutó el VM de Spotify **1.2.92.148** de forma reproducible y se siguió su
salida hasta el generador de bloques de audio. Un snapshot de este generador
reprodujo exactamente un bloque natural. Se puede reconstruir el runtime y
repetir los fixtures sin otro cambio de canción, dentro del proceso ya inicializado.

**En la etapa token E no se identificó una AES extraída y validada.** El candidato de 16 bytes es
estable pero da 0/11 coincidencias. El servicio HTTP no está validado, el mismatch
E/148 no está demostrado y el E2E en ESP32 sigue pendiente. El siguiente paso
está en [PLAN.md](../PLAN.md), no en las antiguas instrucciones de desplegar Flask.

## Recorrido de la etapa token E

| Etapa | Hallazgo / resultado |
|---|---|
| Relectura tras cambios de otro agente | Se retiró la afirmación sin evidencia de «11 vectores pasan»; se conservaron cambios ajenos |
| Conectividad | Primer timeout sin VPN; luego acceso recuperado a Windows por SSH |
| Preflight | EXE/DLL 148; el servidor encontrado era `flask_frida_667.py` con otros offsets; se detuvo |
| Servidor 148 original | Se copió sin cambios y dio HTTP 500 / `Error: system error`; se detuvo |
| Excepciones | `exceptions: propagate` permite la ejecución nativa; el buffer final variable falla como control AES |
| Hooks internos | `setImmediate` + `traps: all` hacen visibles las llamadas repetidas; candidato estable en `0x49f854` |
| Copias | Incluir fuentes de copias de 8 bytes fue necesario para cubrir la forma del hook de referencia; ninguna AES conocida encontrada |
| Contexto de licencia | Se capturaron recurso, respuesta, entrada, candidato y callback, sin headers de cuenta |
| Reproducción correcta | El usuario aclaró que antes cambiaba canciones en Linux; al hacerlo en Windows apareció otro callback |
| Ruta de audio | `0xd8dfb8` lleva a `0xd9e2e4` y `0xd9d0f0`; la precarga usa `0x63fda4` |
| Runtime fresco | `0x4a0268` con callback local deshabilitado reproduce candidatos y bloques de 13 casos, dos veces |
| Control final | Bloque natural y replay: `b323946840ed95fc36fe2019d8d45887` |

Informes y alcance de cada ensayo: [VALIDATION_148_2026-09-24.md](VALIDATION_148_2026-09-24.md).
Contratos y teoría: [PLAYPLAY_ARCHITECTURE_NOTES.md](PLAYPLAY_ARCHITECTURE_NOTES.md).
Descartes: [DECISIONS_148.md](DECISIONS_148.md). Scripts: [catálogo](../tools/README.md).

## Entorno histórico al cerrar la etapa token E

- Host `CZC148B0HC`, `10.16.150.154` vía VPN; SSH `francisco.herrera`, clave
  `~/.ssh/win_claude`. [Acceso y reversión](../../acceso-ssh-pc-windows.md).
- Spotify principal PID **64216**, sesión gráfica **2**, respondió al último chequeo.
- Python Windows: `C:\Users\francisco.herrera\AppData\Local\Programs\Python\Python312\python.exe`.
- DLL en `%APPDATA%\Spotify`; hashes exactos en [FACTS.md](FACTS.md).
- Frida/psutil disponibles; `cryptography` disponible para la prueba de caché.
- Scripts/fixtures copiados a `C:\Users\francisco.herrera\`; se ejecutaron mediante
  SSH en primer plano. `Start-Process` no mantuvo vivo el servidor en el ensayo.
- Todos los validadores/hook sessions terminaron y se desadjuntaron. **No queda
  una captura esperando canción.** Se detuvieron los servidores de prueba y no
  se restauró el servicio 667. Reconfirmar procesos/puertos antes de una nueva sesión.
- No se cambió el cliente instalado, no se copiaron credenciales de cuenta y no
  se editó C++ durante esta continuación. El índice Git preexistente no se alteró.

## Procedimiento histórico para repetir controles E

No es el siguiente paso vigente de extracción; consultar PLAN y la advertencia
de referencias inyectadas antes de reutilizar ese runner.

1. Leer instrucciones y documentos en el orden del [README](../README.md).
2. Comprobar PID, versión y hash antes de usar RVAs. Las firmas de los JS no
   sustituyen el hash completo; algunas funciones solo tienen prólogos comunes.
3. Inspeccionar y ejecutar `check_key_pipeline_148.js` con el runner y fixture
   indicados en el catálogo. No hace falta una nueva reproducción para este ensayo.
4. Si se necesita una captura natural: adjuntar, confirmar evento `attached` y
   recién entonces pedir reproducción **local en Windows**. Distinguir precarga
   de reproducción; cambiar canción en Linux no dispara necesariamente el hook.
5. Investigar el contexto/counter del generador con el control positivo disponible.
   Un archivo `.file` de caché no se ha asociado aún a la licencia capturada.

## Trabajo previo que no debe confundirse con esta sesión

Los arreglos HMAC (`DigestCrypto` con HMAC habilitado) en `Authenticator.h` y
`ApConnection.cpp` ya existían. El C++ conserva `FORCE PLAYPLAY`, volcado de
credenciales, parser manual y un hilo de prueba. Hay que limpiarlos con el plan
aprobado, pero no son una explicación demostrada de las diferencias criptográficas.
Los intentos `LoadLibrary`/Unicorn y el prototipo Flask forman parte de la historia;
no equivalen a un deobfuscador listo. Se conservaron sus archivos para diagnóstico.

## Hipótesis descartada

La argumentación anterior sobre AES-256 incompatible se conserva en
[historia](history/2026-09-24-aes256-hypothesis.md), explícitamente rechazada.
El control token148/v5 valida contenido AES-128-CTR; no implica AES16 extraída.

## Cierre posterior — prueba token148/v5, 2026-09-24 17:00 UTC

El [informe puntual](TOKEN148_ONESHOT_2026-09-24.md) sustituye las conclusiones
criptográficas de Phase2: dos recursos, 4096 bytes por recurso, stream nativo
idéntico a AES-128-CTR; dos variantes b4_seq y dos ejecuciones por variante.
El contenido descifra con CRC Ogg/Vorbis válido. La AES16 sigue sin extraerse.

No fue necesario iniciar cspot: session.json permitió autenticar peticiones
desde Linux. No se copiaron credenciales a Windows ni se inició servicio LAN.
Se conservaron los cambios externos, incluido el token en SpClient.cpp, y el
índice Git. Nota de extracción restaurada por el usuario durante esta sesión.

Postflight: PID72132 responde, sin runners propios activos ni listener8080.
Scripts y fixtures en `%USERPROFILE%\pp_token148_20260924T164814Z` para repetir.
Próximo paso: extraer AES16 usando este control positivo; no cambiar de build
ni asumir un cifrado incompatible a partir de los negativos anteriores.

## Explicación y evaluación de factibilidad — continuación documental

A pedido del usuario se agregó [MECANISMO_Y_EXTRACCION_AES.md](MECANISMO_Y_EXTRACCION_AES.md):
glosario, diagrama licencia→stream→contenido, diferencia entre claves conocidas
y extracción, factibilidad y propuestas ordenadas. La estimación60–80% es juicio
subjetivo para un extractor del build148 bajo el entorno actual, no un resultado
experimental ni una estimación de portabilidad a ESP32.

Hallazgo metodológico: el runner actual inyecta las AES de referencia en
`vectors` y `diagnosticData`. Una futura búsqueda global podría detectar esas
copias. Se propone separar capturador y verificador, con procedencia de datos y
proceso limpio para búsquedas generales. Esto no invalida la comparación de
stream ya realizada: la llamada nativa no recibe las AES de referencia.

Se contrastaron conceptos con FIPS197, SP800-38A y el trabajo original de DCA
de Bos y colaboradores; enlaces en el documento. Esta continuación modifica
solo documentación. No se ejecutaron nuevos experimentos ni se accedió a Windows.
