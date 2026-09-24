# Contexto previo de la investigación

Este documento resume antecedentes; el estado operativo vigente está en
[PLAN.md](../PLAN.md) y en la [validación 148](VALIDATION_148_2026-09-24.md).
No se volvió a verificar información externa durante la revisión documental.

## Por qué PlayPlay

La cuenta del usuario recibe error de audio key legacy (`0x0C` → `0x0E`) en
cspot/ESP32 mientras funciona en clientes oficiales. La investigación previa
comparó cuentas y el eSDK: el mismo harness reproduce con una cuenta habilitada
y no entrega audio con la cuenta afectada. El resultado posterior del Sangean
real fue también silencio, corrigiendo la observación inicial de que reproducía.
Ver [eSDK: resultados](../../esdk-emulation/README.md#what-we-learned-why-this-is-a-museum-piece-not-a-product).
No se retoma la búsqueda de credenciales partner como siguiente tarea.

El issue de referencia es [librespot #1649](https://github.com/librespot-org/librespot/issues/1649).
Su estado/conteo actual de comentarios no forma parte de la evidencia local.
Los experimentos acotan la vía legacy para esta cuenta; no permiten afirmar una
imposibilidad universal para toda cuenta, credencial o futura versión del servicio.

## Request y transformación son problemas distintos

Los barridos guardados/documentados dieron HTTP 200 para token E en v2..v5;
cspot usa E/v5 y se documentó HTTP 200 en hardware. Las otras combinaciones
probadas fueron rechazadas. Esto no garantiza que cualquier token, versión o
credencial futura se comporte igual. La respuesta contiene una clave ofuscada;
obtenerla no acredita la AES que necesita el descifrador.

Los fixtures reúnen 11 pares de AES conocidas y claves ofuscadas de E para
3 recursos. Cambiar `version` cambia la clave ofuscada del mismo recurso.
Eso obliga a informar resultados por versión. El algoritmo estático gen-B
ensayado dio 0/11 para E y 0/5 para C; ese algoritmo concreto quedó descartado.
No demuestra que ninguna implementación estática sea posible.

## Enfoques y binarios

`unplayplay` usa Unicorn con constantes/hook específicos de builds 483/485.
La referencia Wavee motivó intentar llamadas nativas en Windows. El harness
`LoadLibrary` standalone ensayado no cargó el entorno necesario. La ruta que
sí ejecuta las funciones es **Frida dentro del Spotify oficial inicializado**.
Unicorn queda como alternativa futura; no es obligatorio para continuar.

Los instaladores 483/485 extraídos dieron hashes distintos a los fijados por los
paquetes. Saltar el hash gate y conservar RVAs canónicos produjo fallos de memoria.
Esto prueba que esas combinaciones no son válidas, no que todo instalador full
sea inútil ni que el único origen posible de un DLL exacto sea una actualización
delta. Cada binario se identifica por hash y layout.

También se conservó 667 como candidato por referencias históricas a Wavee.
No hay prueba local de que sea el par de E ni RVAs de extracción validados para
ese DLL. Buscar el algoritmo privado de Wavee en sus stubs públicos no produjo
la implementación necesaria; la forma del config y el token del fork sí sirvieron
como antecedentes. Referencias/versiones históricas en [FACTS.md](FACTS.md).

## Qué cambió el 2026-09-24

Antes se atribuía el silencio E2E a incompatibilidad de token/build. Las pruebas
mostraron primero un error de excepciones y después una lectura no validada del
buffer final. Se obtuvo ejecución repetible y un control natural del generador,
pero no una AES en claro. Esa incompatibilidad sigue siendo una hipótesis.
El próximo experimento debe resolver la relación entre contexto codificado y
AES/contenido; no repetir los atajos descartados como si fueran hechos pendientes.
