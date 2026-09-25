# Extracción directa AES: evaluación, preguntas y plan — 2026-09-25

## Alcance y conclusión

Evaluación estática de another-unplayplay y síntesis de las preguntas del usuario.
No se ejecutaron nuevos ensayos Windows ni búsquedas de memoria en esta revisión.
La extracción directa en nuestro build **1.2.92.148 sigue siendo una hipótesis**.
Tenemos una extracción DFA comprobada que puede servir como referencia independiente.

Upstream revisado: commit `24d3223eee0f2c4b6a55f41fcdfb7249406c6fb0`.
Sus [constantes](https://github.com/cycyrild/another-unplayplay/blob/24d3223eee0f2c4b6a55f41fcdfb7249406c6fb0/src/unplayplay/consts.py)
apuntan a **1.2.88.485**, hash `ed3b378d428c8b1034203d62676a0e77cdae157ef70acbbd30be1ba08b8fd045`.
La referencia local al 483 corresponde a una revisión anterior. El README advierte
que la versión del proyecto fue bloqueada por Spotify. No se comprobó aceptación
actual de tokens mediante solicitudes nuevas.

## Preguntas y respuestas

### ¿Encontró una forma directa de obtener la AES? ¿Cómo?

Sí, su código carga una DLL de hash exacto en Unicorn, inicializa la VM en RVA
`0x3e42ac` y llama a la transformada en RVA `0x3e6398`. Un hook en RVA
`0x4129e0` lee RDX como puntero, copia 16 bytes y detiene la emulación.
Ejecuta el código original; no publica una fórmula independiente equivalente.
El código muestra el punto de captura, pero no cómo lo descubrió el autor.
Los cinco vectores de sus tests no se ejecutaron durante esta revisión.

Fuentes: [KeyEmu](https://github.com/cycyrild/another-unplayplay/blob/24d3223eee0f2c4b6a55f41fcdfb7249406c6fb0/src/unplayplay/key_emu.py),
[tests](https://github.com/cycyrild/another-unplayplay/blob/24d3223eee0f2c4b6a55f41fcdfb7249406c6fb0/tests/test_keyemu.py).

### ¿Qué hace runtimefunction_data.py? ¿Por qué lo definió?

Es una tabla de metadatos del binario: límites de funciones, información para
restaurar pila/registros y mapas de estados, regiones try y manejadores catch.
No contiene las claves AES. El emulador intercepta `_CxxThrowException` y utiliza
estos datos, junto con `throwinfo_data.py`, para buscar un manejador compatible
y transferirle la ejecución. Suple parte del runtime Windows que Unicorn no ofrece.
No es una implementación universal de todas las excepciones de Windows.

Fuentes: [state_builder](https://github.com/cycyrild/another-unplayplay/blob/24d3223eee0f2c4b6a55f41fcdfb7249406c6fb0/src/unplayplay/seh/state_builder.py),
[dispatcher](https://github.com/cycyrild/another-unplayplay/blob/24d3223eee0f2c4b6a55f41fcdfb7249406c6fb0/src/unplayplay/seh/dispatcher.py).

### ¿No tener esas tablas nos limita? ¿Siempre lanza una excepción y se recupera?

Si la ruta ejecutada necesita excepciones C++, su manejo es necesario para
continuar. Se pueden extraer los metadatos pertinentes de nuestro binario;
los del 485 no son trasladables cambiando solamente la base.
Si la ruta no necesita excepciones, las tablas no son requisito para esa ruta.
Es plausible que el autor las necesitase para una ejecución normal, pero no
observamos trazas que demuestren que cada extracción lanza una excepción.
No se trata de ignorar un error: su código reproduce la transferencia al catch
previsto por el binario. Tampoco demostramos que SEH sea nuestro bloqueo actual.

### ¿Cómo obtenemos nosotros la llave?

El servicio recibe `obfuscated_key` y `b4_seq`, ejecuta con Frida la cadena
nativa del 148, obtiene descriptor28 y contexto740, y captura un bloque correcto.
Restaura el contexto antes de cada uno de 16 fallos controlados, XOR1 en offsets
`0xa0..0xaf`. El solver DFA recupera K10 y revierte la expansión AES-128 para
obtener K0. Comprueba `AES_ECB(K0, IV) == bloque nativo`; los ensayos también
validaron prefijos Ogg/Vorbis con CRC. No implica que K0 esté almacenada en claro.

Fuentes: [servicio](HTTP_DFA_SERVICE.md),
[pruebas](../runs/20260925-http-dfa-service/EXPERIMENT.md),
[Unicorn](UNICORN_FEASIBILITY_148.md).

### ¿Podemos buscar una AES conocida para localizar la dirección?

Sí. Una coincidencia puede conducir a la instrucción que produce o consume la
clave. La dirección del buffer puede cambiar por asignación y ASLR: el resultado
reutilizable es el punto de código, su RVA y su contrato de captura.
La referencia debe permanecer fuera del proceso observado. Introducirla como
patrón de búsqueda en un script Frida también puede contaminar el espacio de
memoria, aunque no se pase a una función de Spotify. Preferir capturas sin claves
de referencia y comparación offline en otro proceso.

### ¿Puede estar fragmentada o en otras posiciones?

Sí, como hipótesis: dos fragmentos de 8 bytes, registros, claves de ronda o una
representación codificada. Buscar primero K0 y K1..K10 completas; luego fragmentos
de 8 bytes y disposiciones/órdenes derivados de instrucciones observadas. Los
fragmentos cortos producen falsos positivos; una coincidencia aislada no prueba
procedencia. K10 completa permite invertir el key schedule; un fragmento arbitrario
no basta. La ausencia de coincidencias no prueba ausencia global: la clave podría
ser transitoria o no existir nunca sin codificar.

## Viabilidad del port

Son reutilizables los conceptos de sesiones Unicorn, llamadas Win64, asignador
y manejo de excepciones, sujetos a adaptar contratos y metadatos. Cambiar RVA y
token no alcanza. Nuestro candidato16 en `0x49f854` no validó como AES en los
controles; descriptor28 y candidato16 no deben etiquetarse como llaves.

El par comprobado localmente es **token148/v5 + build148**, conservando `b4_seq`.
HTTP200 con otro token no demuestra compatibilidad criptográfica. Ver
[FACTS](FACTS.md) para hash exacto, constantes y pruebas.

Unicorn ya ejecutó generador/DFA desde snapshots de dos recursos, pero no produjo
el contexto desde una licencia en una sesión vacía. Quedan dependencias de
mapeo, imports y runtime. El dump está relocalizado y tiene layout RVA=offset;
no debe cargarse como PE original ni moverse sin reparar referencias absolutas.
La extracción directa nativa y la emulación autónoma son objetivos distintos:
encontrar un hook no resuelve automáticamente el arranque del emulador.

## Plan de acción: buscar una captura directa en 148

1. **Preparar una corrida reproducible.** Leer las reglas, arquitectura y
   [playbook](RVA_DISCOVERY_PLAYBOOK.md); inspeccionar scripts antes de usarlos.
   Crear `runs/<id>/` con fuentes, manifiesto y resultados nuevos. Registrar
   versión/hash, PID, base y estado del servicio mediante preflight. Un solo
   operador Windows/Frida; evitar instrumentación concurrente con el worker.
   Si hace falta suspenderlo, usar parada ordenada y restaurar su operación.
2. **Establecer controles independientes.** Obtener referencias AES por DFA fuera
   del proceso que se observará; iniciar una sesión limpia para la captura.
   Mantener AES y claves de ronda en el verificador externo. No usar como
   extractor el runner histórico que inyecta referencias. Documentar cualquier
   límite para demostrar ausencia de contaminación previa.
3. **Capturar ventanas acotadas.** Seguir los contratos validados de entrada y
   retorno de transformada `0x49eaa4`, copia de descriptor al retorno `0x4a0422`,
   candidato `0x49f854`, inicialización `0xd9e2e4` y generador `0xd9d0f0`.
   Capturar registros y buffers cuya validez/tamaño se conozca, antes de que
   termine su vida útil; filtrar hilo y asociar eventos a la licencia. Empezar
   por regiones justificadas; ampliar la cobertura según resultados y registrar
   exactamente qué regiones y momentos se examinaron.
4. **Buscar offline.** Comparar K0 y K1..K10 completas en capturas limpias. Después
   buscar fragmentos de 8 bytes y variantes justificadas por el código, no una
   búsqueda combinatoria indiscriminada. Revisar registros SIMD si la ruta los
   usa. Documentar también negativos con su cobertura temporal y espacial.
5. **Atribuir cada coincidencia.** Identificar instrucción escritora/consumidora,
   módulo/RVA, hilo, buffer y vida útil. Correlacionar fragmentos por flujo de
   datos; demostrar orden y reconstrucción. Repetir con al menos dos llaves
   distintas y otro proceso/base. Descartar copias del verificador o instrumento.
6. **Validar un extractor independiente.** Si aparece un punto candidato, crear
   una captura que no reciba AES esperada ni use DFA para producir la salida.
   Comparar después contra DFA, bloque nativo y contenido con CRC. Validar una
   licencia nueva fuera de los controles. Una captura de K10 más inversión del
   key schedule debe describirse así, distinta de una lectura directa de K0.
7. **Cerrar con evidencia y siguiente decisión.** Éxito: entrada licencia → AES
   verificada mediante hook/reconstrucción, repetible sin referencia suministrada.
   Resultado negativo: límites de búsqueda y dependencias, sin afirmar que la
   extracción directa es imposible. Mantener DFA como vía operativa. Evaluar el
   port Unicorn por separado, con los criterios de su informe de viabilidad.

## Instrucción lista para otro agente

> Continúa la investigación de extracción directa AES para Spotify 1.2.92.148.
> Lee AGENTS.md, research/playplay/AGENTS.md y el contexto mínimo que indican;
> después lee research/playplay/docs/DIRECT_AES_SEARCH_148.md y sus fuentes
> pertinentes. Ejecuta progresivamente el plan documentado, empezando por
> preflight y controles sin contaminación. Usa nuestro DFA como referencia
> externa y busca K0, claves de ronda y, después, fragmentos justificados en
> capturas/registros. No introduzcas las AES esperadas en el proceso observado.
> Identifica y valida el RVA productor/consumidor de cualquier coincidencia.
> Puedes realizar la instrumentación Windows necesaria por el SSH documentado,
> siendo el único operador y coordinando la parada/restauración ordenada del
> servicio. Conserva cada ensayo y sus fuentes en una corrida nueva; no alteres
> evidencia anterior ni cambios ajenos. No cambies la integración de cspot.
> Actualiza documentación con resultados, cobertura y bloqueos. No declares
> extracción directa hasta validarla con claves distintas, reinicio y una
> licencia nueva sin proporcionar la AES al extractor. Si no aparece, informa
> qué se descartó realmente y el siguiente experimento concreto.
