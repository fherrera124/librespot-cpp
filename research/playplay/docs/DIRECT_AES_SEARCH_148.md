# Búsqueda directa de AES en Spotify 1.2.92.148 — cierre

**Resultado:** no se encontró K0 ni K1..K10, completas (16 bytes) o en mitades
contiguas de 8 bytes, en los buffers capturados para dos licencias. Los dos
bloques nativos coinciden con `AES-ECB(K0, IV)` usando las referencias DFA.
La búsqueda directa queda cerrada con resultado **negativo acotado**; DFA sigue
siendo el método operativo. Este resultado no demuestra ausencia de K0 en todo
el proceso ni en otros instantes o representaciones.

## Datos y comprobación

El [anexo crudo](DIRECT_AES_148_DATA.json) conserva, sin registros de eventos,
los bytes necesarios: entrada e inicializador, candidato16, dos descriptores28
por licencia, contexto740, bloque16, fuentes únicas de las 62 copias observadas,
y una copia de las regiones VM de 3072 y 512 bytes que fueron iguales para
ambas licencias. Incluye hashes de los informes y fuentes originales. Las AES
de referencia permanecen en los dos fixtures de licencia enlazados desde el
anexo; no entraron en el proceso observado. SHA256 del anexo:
`80c72c87cb118ec77d57314d45920d7dfa84f04a0f7cd2341637d9f48dad8d7b`.

Para repetir la comparación offline:

```sh
python3 research/playplay/tools/check_direct_aes_148.py
```

El verificador comprueba los hashes de los fixtures, la expansión AES-128 con
un vector conocido y ambos bloques nativos. Luego busca K0..K10 en todas las
ventanas solapadas de 16 y 8 bytes del anexo. Resultado: 2/2 bloques válidos;
0 coincidencias en 8675 ventanas de 16 bytes y 9275 de 8 bytes. El recuento
incluye la región VM compartida frente a las dos AES y deduplica las fuentes
de copia repetidas.

## Interpretación

- El desensamblado del dump 148 añade cuatro referencias de código
  ([mapa de RVAs](FACTS.md)):
  `0x49f627` llama a `0x49f854`; `0x49f8ea` copia 3072 bytes desde RDI;
  `0x49f925` copia otros 16 bytes desde R12, cuyo contenido no se atribuyó.
  RDI y R12 provienen de `args[0]` y `args[3]`, respectivamente. Además,
  `0x49f894` llama a `0x49f994`, que lee `gs:[0x58]`. `0x49f904` y
  `0x49f961` ya estaban documentados. Ninguno es un punto validado de K0.
- Los 3072 bytes leídos desde `args[0]` al entrar en `0x49f854`, así como los
  512 bytes siguientes, son idénticos para las dos licencias. La función copia
  3072 bytes desde ese puntero. La igualdad descarta que **esos bytes capturados**
  contengan una clave lineal diferente por licencia; el tamaño por sí solo no
  identifica tablas AES ni un key schedule codificado. El tamaño real y la
  pertenencia de los 512 bytes siguientes al objeto VM no están demostrados.
- El candidato16 cambia con la licencia y difiere de la entrada ofuscada y de
  K0. El contexto740 también depende de la licencia; los descriptores28 varían
  entre ejecuciones de la misma entrada. Ninguno de esos buffers mostró una
  clave de ronda completa o media clave en las ventanas examinadas.
- La firma del RVA `0x49eaa4` empieza `40 53`: `0x40` es un prefijo REX y
  `0x53` es `push rbx`. ASLR cambia la dirección de carga, no estos bytes.
- La captura de buffers acotados usó el preflight de versión/hash del build.
  La captura posterior del objeto VM validó solo los dos bytes `40 53`; su
  informe anotó versión/hash esperados como constantes. El anexo conserva el
  hash de esa fuente, sin atribuirle una verificación de disco que no hizo.

[`another-unplayplay`](https://github.com/cycyrild/another-unplayplay/blob/24d3223eee0f2c4b6a55f41fcdfb7249406c6fb0/src/unplayplay/key_emu.py)
muestra un hook de 16 bytes para su build 1.2.88.485.
No se validó un hook equivalente en 1.2.92.148 ni una dirección de memoria
que contenga K0. Las direcciones de buffers pueden variar entre procesos.

## Propuestas si se reabre

1. Seguir la transformación aún no observada dentro de `0x4af25c` (llamada
   desde `0x49f94e`) y la inicialización `0xd9e2e4`. Capturar registros
   generales/SIMD y buffers de tamaño comprobado antes y después de las
   instrucciones que escriben el descriptor y el contexto; las copias ya
   examinadas en `0x49f904` y `0x49f925` no aportaron una coincidencia.
2. Seguir la lectura TLS de `0x49f994` y los accesos posteriores de `0x49f854`
   para identificar buffers y offsets efectivos, sin presumir que allí esté K0.
3. Comparar las capturas **fuera de Spotify** con K0..K10 obtenidas por DFA.
   Una coincidencia debe atribuirse a la instrucción productora o consumidora,
   RVA y offset del buffer, y repetirse con otra licencia y otro proceso/base.
   Verificar build/hash antes de instrumentar. Si no hay coincidencia, registrar
   regiones e instantes examinados; K0 podría no existir linealmente en memoria.
