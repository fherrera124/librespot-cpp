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

## Organización del workspace para agentes — 2026-09-24

Se unificaron instrucciones en AGENTS raíz/PlayPlay y entradas Gemini/Claude.
STATUS es la síntesis vigente; README, PLAN y task quedan acotados. La hipótesis
AES256 rechazada y el checklist anterior se archivaron con etiquetas explícitas.
Los seis scripts externos de captura se agruparon en tools/token_capture_148;
los Python resuelven JS por __file__. Nota externa movida a docs/external sin
alterar bytes; registro de movimientos en runs/tree-migration-20260924.json.

Catálogo:15 grupos de recursos,32 documentos mantenidos,2 manifiestos con62
artefactos (60 existentes intactos y2 snapshots de fuente del control nativo).
El checker offline verifica rutas, enlaces, tamaños y SHA256. Sus cuatro pruebas
pasaron y la receta offline de contenido volvió a pasar4/4 casos. Sintaxis de
runners reubicados y TOML de perfiles comprobadas; git diff --check limpio.
SpClient.cpp y el índice Git coinciden con el inicio de esta reorganización.
No se ejecutaron capturas nuevas ni se accedió a Windows.

Modelos efectivos: gpt-6-luna/medium hizo inventario de solo lectura;
gpt-6-sol/medium auditó el handoff e implementó el checker con pruebas. El
coordinador integró y validó. La segunda revisión delegada a Sol no terminó
por límite de uso; el coordinador realizó la revisión final. No se atribuye
esa revisión al agente ni se afirma ahorro medido de tokens/costo.

Perfiles persistentes preparados en .codex/agents: inventario Luna, implementación
Sol y revisión difícil Astra. TOML válido; carga efectiva depende del cliente.
Reglas y contratos en docs/AGENT_WORKFLOW.md. El siguiente trabajo experimental
sigue siendo el capturador limpio de PLAN; esta organización no extrae AES16.

En el último chequeo apareció un cambio concurrente en tools/test_dll.cpp
con whitespace en línea28, ajeno a esta reorganización. Se conservó; el diff
del resto del workspace pasó la comprobación.

## Capturadores recuperados y procedencia de RVA — 2026-09-24

Se verificaron los dos JS recuperados: añadir exactamente un LF final reproduce
los hashes de clean_capture_report y context_dump históricos. Antes de editarlos
se preservaron esas fuentes en
[manifiesto de recuperación](../runs/20260924-clean-capture-recovery/manifest.json).
Los informes originales permanecen intactos. Los dos primeros bloques del
informe de contexto coinciden con AES independiente; la revisión anterior también
reprodujo offline el caso DFA y descifró el prefijo4096 con CRC Ogg válido.

Las herramientas actuales usan un helper de preflight: ruta del módulo vivo,
Windows x64, versión fija, hash de disco y firmas de cinco entradas antes de hooks.
El capturador de contexto libera su hook en finally; el runner trata errores de
payload, timeouts e interrupciones como fallos y libera script/sesión. Se preserva
la distinción entre hash de disco, firmas en memoria y control criptográfico.

Ocho tests offline pasaron, incluidos tres de comportamiento JS con quickjs.
Comando: `PYTHONPATH=/tmp/playplay-js-audit-20260924 python3 research/playplay/tools/test_clean_capture.py -v`.
QuickJS se instaló para validación en esa carpeta temporal, sin incorporarlo como
dependencia del capturador. El checker de catálogo/enlaces/hashes pasó. El check
global de whitespace sigue señalando el cambio ajeno de test_dll.cpp; no se editó.
No se accedió a Windows ni se modificó el índice; falta corrida de integración
de esta revisión protegida. No quedaron procesos de ensayo remotos propios.

Se amplió [RVA_DISCOVERY_PLAYBOOK](RVA_DISCOVERY_PLAYBOOK.md) con reconstrucción
histórica, separación VM/generador, evidencia disponible y procedimiento para
otras versiones. No se inventó el origen ausente de49cb88/49eaa4 ni una función
dedicada de reparación. La guía oficial OpenAI Docs sobre AGENTS.md se usó para
la organización entre agentes, no como fuente de hechos de Spotify.
gpt-6-luna/medium hizo una lectura independiente y acotada de procedencia;
el coordinador implementó y comprobó los cambios. Uso y límites en
[CLEAN_CAPTURE_148](CLEAN_CAPTURE_148.md).

## Evaluación local de unicorn_harness — 2026-09-24

Se preservó el prototipo externo intacto y se hicieron pruebas acotadas con
Unicorn2.1.4, sin APIs genéricas exitosas ni páginas inventadas. Stream desde los
dos contextos guardados:4/4 bloques correctos al ensayar dos bases. DFA con16
fallos por recurso:2/2 AES recuperadas; prefijos4096 descifrados y CRC Ogg válido.
Esto elimina la necesidad de proceso vivo para esa etapa, pero conserva como
entrada un snapshot por recurso. No demuestra construcción desde licencia nueva.

Las pruebas de init/constructor registraron dependencias de GS/TEB, ruta AVX de
copia, direcciones externas y límites de ejecución; no hubo cadena completa.
Se confirmó error de ABI en el prototipo y discrepancia de base: el header del
dump exacto contiene0x7ff9b7da0000. Se corrigió la tabla FACTS. No se reutilizaron
constantes de485 ni se ejecutó el `LoadLibrary` C++ externo.

Evidencia, fuentes e inputs con hashes en
[manifiesto](../runs/20260924-unicorn-feasibility/manifest.json);
[dictamen y propuestas](UNICORN_FEASIBILITY_148.md). Auditoría estática acotada:
gpt-6-luna/medium. Pruebas y revisión: coordinador. No se accedió a Windows,
Spotify ni credenciales; sin procesos remotos creados ni cambios al índice Git.

## Pausa solicitada y cierre — 2026-09-24

El usuario solicita commitear la investigación y los avances y detener el trabajo
por el momento. Se conservan resultados, fuentes, manifiestos y pendientes. El
alcance validado es recuperación DFA y control de contenido para casos guardados;
la extracción autónoma desde una licencia nueva en Unicorn sigue pendiente.
STATUS, PLAN y task señalan la pausa. No se iniciaron nuevos experimentos para
este cierre. Los prototipos externos ya staged se conservan como investigación;
su inclusión en el commit no acredita madurez de producción ni resultados nuevos.

## Servicio HTTP AES16 y cliente cspot — 2026-09-25

Por pedido del usuario se retomó la implementación: API Frida/Flask que recibe
`obfuscated_key` y `b4_seq` obligatorios, recupera AES16 por DFA y la verifica
contra el stream nativo. Worker separado con timeout, rechazo de concurrencia,
puerto exclusivo, health y parada ordenada. El cliente C++ conserva ambos campos,
usa JSON estricto, URL/token configurables y timeout de 25 s; se retiró el
volcado de credenciales. La CLI compiló; suite doctest bloqueada por submódulos
de test ausentes. Ocho pruebas Python locales y siete Windows pasaron.

Dos licencias guardadas pasaron repetidas extracciones HTTP y descifrado de
prefijos con CRC. También se probó con Spotify recién abierto y tras parada y
arranque ordenados, conservando el PID de Spotify. Se corrigió el arranque
inicial por wrapper tras detectar procesos residuales y un fallo registrado
en frida-agent.dll. El historial negativo se conserva, sin atribuir causa no
aislada. Diez archivos finales coinciden por SHA256 entre repo y Windows.

Queda activa la tarea a demanda CspotPlayPlayDfa y el túnel local SSH8765.
El coordinador fue el único operador Windows; gpt-6-sol/medium completó cliente,
parser y compilación. No se tocaron procesos ajenos ni el índice Git.
Pendiente: reproducción completa y licencia nueva fuera de los dos controles.
[Guía](HTTP_DFA_SERVICE.md), [corrida](../runs/20260925-http-dfa-service/EXPERIMENT.md).

## Búsqueda directa AES en 148 — cierre 2026-09-25

Dos licencias: los bloques nativos coincidieron 2/2 con AES(K0, IV); ninguna
K0..K10 completa o en mitades de 8 bytes apareció en las ventanas capturadas.
Los bytes crudos necesarios y sus límites están en
[el cierre del análisis](DIRECT_AES_SEARCH_148.md). DFA permanece operativo.
