# librespot-cpp — componente ESP-IDF

Wrapper de `cspot`/`bell` como componente ESP-IDF instalable vía
[Component Manager](https://docs.espressif.com/projects/idf-component-manager/en/latest/).
No trae glue de aplicación (WiFi, zeroconf, pines I2S) — eso le
corresponde al proyecto consumidor; este componente expone el motor
(`Session`, `DealerClient`, `ConnectStateHandler`, `TrackPlayer`,
`TrackQueue`, `AudioSink`) listo para linkear.

## Uso

En el `idf_component.yml` del componente que lo consuma:

```yaml
dependencies:
  librespot_cpp:
    git: "https://github.com/fherrera124/librespot-cpp.git"
    path: "targets/esp32"
```

Luego, en el `CMakeLists.txt` de ese mismo componente:

```cmake
idf_component_register(
    SRCS "my_connect.cpp"
    INCLUDE_DIRS "include"
)
```

No hace falta declarar `REQUIRES librespot_cpp` explícito — el Component
Manager ya agrega las dependencias del manifest como requisito del
componente.

## Opciones (`idf.py menuconfig` → "librespot-cpp (Spotify Connect engine)")

Codecs (`LIBRESPOT_CODEC_VORBIS`/`MP3`/`AAC`/`OPUS`/`ALAC`), y toggles para
el MQTT/HTTP-server internos de `bell` y el backend JSON (cJSON vs
nlohmann_json) — ver `Kconfig` en este directorio para los defaults.
