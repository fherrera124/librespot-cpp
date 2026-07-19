# cspot/bell — Spotify Connect engine

Motor (`Session`, `DealerClient`, `ConnectStateHandler`, `TrackPlayer`,
`TrackQueue`, `AudioSink`) sin glue de aplicación (WiFi, zeroconf, pines
I2S) — eso le corresponde al proyecto consumidor.

## Consumo en CMake plano (Linux/Apple/Win32, o `targets/cli`)

`add_subdirectory()` normal — ver `targets/cli/CMakeLists.txt` como
referencia.

## Consumo en ESP-IDF, vía Component Manager

`cspot/CMakeLists.txt` es dual-mode: bajo `ESP_PLATFORM` se registra como
componente real (`idf_component_register()`) en vez de un `add_library()`
plano. En el `idf_component.yml` del componente que lo consuma:

```yaml
dependencies:
  librespot_cpp:
    git: "https://github.com/fherrera124/librespot-cpp.git"
    path: "cspot"
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

### Opciones (`idf.py menuconfig` → "librespot-cpp (Spotify Connect engine)")

Codecs (`LIBRESPOT_CODEC_VORBIS`/`MP3`/`AAC`/`OPUS`/`ALAC`), y toggles para
el MQTT/HTTP-server internos de `bell` y el backend JSON (cJSON vs
nlohmann_json) — ver `Kconfig` en este directorio para los defaults.
