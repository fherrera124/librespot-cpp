# Calidad de audio configurable + colchón de prefetch invariante en segundos

Rama `dev` (motor `cspot`, target ESP32-S3 `JC3248W535`). Documenta el estado
implementado: la calidad de audio ya no está fijada a un único formato, y el
colchón de prefetch/read-ahead se mantiene aproximadamente constante en
segundos sin importar qué calidad termine resolviéndose.

## 1. Selección de calidad

[`FileProvider.cpp`](../main/src/FileProvider.cpp) ya no acepta un único
`AudioFormat` fijo. Recibe una preferencia ordenada
(`Session.h`'s `AudioConfig::qualityPreference`, default
`{OGG_VORBIS_320, OGG_VORBIS_160, OGG_VORBIS_96}`) y selecciona la primera
variante que el track realmente ofrece — no necesariamente la primera
entrada de la lista que Spotify devuelve.

## 2. Colchón de prefetch invariante ante la calidad

El mecanismo de read-ahead (`ChunkCache`, `PrefetchWorker`,
`chunksToPrefetch()`) razona en cantidad de chunks, y el tamaño de cada
chunk es **fijo**:

- `kCDNChunkSize = 32 * 1024` bytes — [`CDNDataStream.h`](../main/include/audio/CDNDataStream.h)
- `kChunkCacheCapacity = 9` — [`CDNDataStream.cpp`](../main/src/audio/CDNDataStream.cpp)

Un `chunkSize` fijo por sí solo haría que la duración real de cada chunk (y
por lo tanto el colchón, `prefetchDepth × duración/chunk`) dependiera de la
calidad — a 320kbps un chunk de 32KB cubre ~1/3 de lo que cubre a 96kbps.
En vez de volver a variar `chunkSize` (lo que también cambiaría el tamaño
de cada request HTTP y el RAM por chunk según la calidad), la variable que
compensa es **`prefetchDepth`**: se deriva por-track en
[`AudioDecoderImpl::openStream()`](../main/src/audio/AudioDecoderImpl.cpp)
a partir de un `targetPrefetchDuration` fijo (`AudioConfig`, default
6500ms) y el bitrate del formato resuelto (`bytesPerSecond()`), redondeando
hacia arriba:

```cpp
size_t targetBytes = targetPrefetchDuration.count() * bytesPerSecond(format) / 1000;
size_t depth = ceil(targetBytes / kCDNChunkSize);
```

Con el target de 6.5s y `chunkSize` de 32KB, esto da:

| Formato | Bitrate | ms/chunk | prefetchDepth |
|---|---|---|---|
| OGG_VORBIS_96  | 12 kB/s | ~2730ms | 3 |
| OGG_VORBIS_160 | 20 kB/s | ~1640ms | 4 |
| OGG_VORBIS_320 | 40 kB/s | ~820ms  | 8 |

`kChunkCacheCapacity=9` cubre el peor caso (320kbps, depth=8) con un slot
de margen — un colchón real de ~6.5-7s en las tres calidades, en vez de
variar entre ~1.6s y ~6.5s como con `chunkSize` fijo sin compensar.

El resultado (depth y duración/chunk derivados) se loguea una vez por
apertura de stream, en `openStream()`.

## 3. `ReadAheadPolicy` simplificado

La interfaz pluggable `ReadAheadPolicy`/`FixedDepthReadAheadPolicy` se
reemplazó por una función libre, `chunksToPrefetch(currentChunkIndex,
depth, totalChunks)` — al pasar `depth` a ser un valor por-track (vía
`PrefetchWorker::Session::depth`) en vez de fijo por instancia del worker,
la abstracción de "política" ya no modelaba nada real. No hay plan de
agregar una segunda implementación (política adaptativa por throughput,
etc. — descartado, ver la nota del punto 4 más abajo).

## 4. Alcance del decoder actual

El decoder es Vorbis-only, no solo la selección de formato:
[`AudioDecoderImpl.cpp`](../main/src/audio/AudioDecoderImpl.cpp) usa
`bell::audio::OggContainer` + `bell::TremorVorbisCodec` de forma directa.
Formatos no-Vorbis (`MP3_*`, `AAC_*`, `FLAC_*`) requerirían un
contenedor/codec distinto, no solo otro valor de `AudioFormat` — fuera de
alcance, igual que una política de read-ahead adaptativa (por throughput,
boost cerca de EOF, etc.): el objetivo del proyecto nunca fue tolerar
degradación sostenida de red, así que ese tipo de complejidad adicional no
está planeada.
