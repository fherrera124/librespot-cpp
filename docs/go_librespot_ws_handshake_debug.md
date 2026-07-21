# Cómo se levantó `go-librespot` para comparar el handshake WS del Dealer (2026-07-16)

Continuación de `docs/dealer_websocket_migration.md` (§28/§31/§33): con el
fingerprint TLS/handshake WS como único candidato real que queda sin
descartar para el cierre del Dealer, este documento registra cómo se armó
un `go-librespot` real, corriendo contra la cuenta real de Spotify del
usuario, con logging agregado en la capa más baja del handshake WS -
para comparar contra `WebSocketTransport.cpp`'s `handshake()` (este
archivo era `EspWebSocketTransport.cpp` cuando se escribió esta
comparación - luego absorbió también la rama host-only, ver
`docs/dealer_websocket_migration.md` §1/§28/§29).

## Entorno usado

Todo vive en el scratchpad de la sesión que lo armó originalmente
(`/tmp/claude-1000/-desarrollo-git-cspot/23bf2341-252f-49ea-a33e-46b274b26e1c/scratchpad/`),
reutilizado tal cual por esta sesión:

- **Go 1.25.5** extraído localmente en `scratchpad/go/` (tarball
  `go1.25.5.tar.gz` bajado ahí mismo) - no depende de un Go del sistema.
- **Fuente**: clon de `https://github.com/devgianlu/go-librespot.git` en
  `/desarrollo/git/go-librespot`, en `master` (HEAD `8331ce9`, confirmado
  en `docs/dealer_websocket_migration.md` §36 que no diverge de la última
  release estable `v0.7.4` en nada relevante a esta investigación).
- **Dependencias nativas** (el binario usa CGO para decodificar Vorbis):
  `libvorbis-dev`/`libogg-dev`/`libflac-dev` no instalables vía `apt` en
  este sandbox (sin privilegios), así que sus `.deb` se descargaron y se
  extrajeron a mano a `scratchpad/golibrespot_deps/prefix/` (headers +
  símlinks `.so` sin versión, necesarios para linkear). Las librerías
  `.so` **con** versión (`libvorbis0a`, `libogg0`, `libflac14`,
  `libvorbisenc2`, `libvorbisfile3`) sí están instaladas por `apt` a
  nivel de sistema - son las que el binario usa en runtime.

### Variables de entorno para compilar

```bash
SCRATCH=/tmp/claude-1000/-desarrollo-git-cspot/23bf2341-252f-49ea-a33e-46b274b26e1c/scratchpad
export PATH=$SCRATCH/go/bin:$PATH
export CGO_ENABLED=1
export PKG_CONFIG_PATH=$SCRATCH/golibrespot_deps/prefix/usr/lib/x86_64-linux-gnu/pkgconfig
export CGO_CFLAGS="-I$SCRATCH/golibrespot_deps/prefix/usr/include"
export CGO_LDFLAGS="-L$SCRATCH/golibrespot_deps/prefix/usr/lib/x86_64-linux-gnu"

cd /desarrollo/git/go-librespot
go build -o $SCRATCH/golibrespot_debug ./cmd/daemon
```

El módulo de Go (`go.mod`) resuelve sus dependencias (incluida
`github.com/coder/websocket`, la librería WS que usa el Dealer) desde el
cache de módulos ya poblado en `~/go/pkg/mod` - no hizo falta red nueva
para este build.

## Configuración (`config.yml`)

```yaml
log_level: debug
log_disable_timestamp: false
device_name: cspot-verificacion
device_type: speaker
audio_backend: pipe
audio_output_pipe: <scratch>/golibrespot_config/audio.pipe
audio_output_pipe_format: s16le
zeroconf_enabled: true
credentials:
  type: zeroconf
  zeroconf:
    persist_credentials: false
external_volume: false
```

`persist_credentials: false` - a propósito o no, el efecto real es que
**cada reinicio del proceso pierde el login** y necesita volver a
emparejarse por Zeroconf desde el teléfono (seleccionar
"cspot-verificacion" en el picker de Connect) para autenticar de nuevo y
abrir una conexión nueva del Dealer - eso es lo que dispara el handshake
que este documento quiere capturar.

## Logging agregado (temporal) - `dealer/dealer.go`

`handshakeLogger` (nuevo `http.RoundTripper` envolviendo el `Transport`
del cliente HTTP que usa `websocket.Dial()` en `Dealer.connect()`) - loguea:

- La request saliente: método, URL (con el `access_token` real
  **redactado** - nunca se loguea un token en vivo), y todos los headers.
- La respuesta entrante: status y todos los headers.

No usa `httputil.DumpRequestOut`/`DumpResponse` (esas funciones hacen su
propia I/O para calcular `Content-Length`, arriesgando una segunda
petición espuria contra un endpoint de upgrade WS) - lee los campos de
`*http.Request`/`*http.Response` directo, sin tocar la red de nuevo.

Es un cambio puramente temporal para esta investigación - **no** se va a
mergear a `go-librespot` ni queda como parte permanente de este clon;
revertir (`git checkout -- dealer/dealer.go` en `/desarrollo/git/go-librespot`)
una vez terminada la comparación.

## Cómo se corrió

```bash
SCRATCH=/tmp/claude-1000/-desarrollo-git-cspot/23bf2341-252f-49ea-a33e-46b274b26e1c/scratchpad
nohup $SCRATCH/golibrespot_debug --config_dir $SCRATCH/golibrespot_config \
    > $SCRATCH/golibrespot_debug.log 2>&1 &
disown
```

El proceso anterior (sin el logging, corriendo desde una sesión previa)
se detuvo primero (`kill`, confirmado explícitamente por el usuario antes
de hacerlo - ese proceso no lo había levantado esta sesión).

Log en vivo monitoreado con el tool `Monitor` de esta sesión, filtrando
por: `HANDSHAKE-DEBUG`, cierre/reconexión de la conexión del Dealer,
emparejamiento Zeroconf, y errores - para capturar tanto el handshake
inicial como, si ocurre durante la prueba, un cierre/reconexión real de
la conexión (igual al patrón que `cspot` viene mostrando desde §22).

## Ver los logs en vivo

```bash
tail -f /tmp/claude-1000/-desarrollo-git-cspot/23bf2341-252f-49ea-a33e-46b274b26e1c/scratchpad/golibrespot_debug.log
```

## Reproducción de audio real (`aplay`)

`audio_output_pipe` en `config.yml` apunta a un FIFO
(`golibrespot_config/audio.pipe`) - si nada tiene el extremo de lectura
abierto, `go-librespot` falla al cargar cualquier track ("failed to open
fifo: ... no such device or address"). Se necesita un lector real del
otro lado del pipe, igual que hizo una sesión anterior (quedó registrado
en `scratchpad/aplay.log`):

```bash
SCRATCH=/tmp/claude-1000/-desarrollo-git-cspot/23bf2341-252f-49ea-a33e-46b274b26e1c/scratchpad
nohup aplay -f S16_LE -r 44100 -c 2 $SCRATCH/golibrespot_config/audio.pipe \
    > $SCRATCH/aplay.log 2>&1 &
disown
```

Formato/sample rate confirmados en `player/player.go`:
`SampleRate = 44100`, `Channels = 2` (y `audio_output_pipe_format: s16le`
en `config.yml`) - tienen que coincidir con los flags de `aplay` o el
audio sale distorsionado/con velocidad incorrecta.

## Handshake capturado - primera conexión real (2026-07-16 09:52:38)

**Request saliente** (`[HANDSHAKE-DEBUG] >>>`):

```
GET https://guc3-dealer.spotify.com:443/?access_token=<redacted> HTTP/1.1
User-Agent: go-librespot/8331ce97 Go/go1.25.5
Connection: Upgrade
Upgrade: websocket
Sec-Websocket-Version: 13
Sec-Websocket-Key: LNXDduHwq5gAOqhW2inJSw==
```

*(la línea `GET https://...` refleja `req.URL.String()` tal como Go lo
representa internamente, no necesariamente el byte a byte real de la
request-line en el wire - un cliente HTTP sin proxy normalmente manda la
forma relativa `GET /?access_token=... HTTP/1.1` + un header `Host:`
aparte, que Go maneja como campo separado (`req.Host`), no listado en
`req.Header` - por eso no aparece en la captura. Esto es una limitación
de cómo se logueó, no evidencia de que falte el header en el wire real.)*

**Respuesta del servidor** (`[HANDSHAKE-DEBUG] <<<`):

```
HTTP/1.1 101 Switching Protocols
Upgrade: websocket
Connection: upgrade
Sec-Websocket-Accept: jcstStfQ4BMfoz/47AgdlZvIFuc=
```

## Comparación contra `WebSocketTransport.cpp::handshake()`

Construcción real de `cspot` (`WebSocketTransport.cpp`, dentro de
`handshake()`):

```
GET <path> HTTP/1.1
Host: <host>:<port>
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: <key>
Sec-WebSocket-Version: 13
User-Agent: cspot/1.0
```

**Diferencias reales encontradas**:
1. **Orden de headers distinto**: go-librespot manda
   `User-Agent → Connection → Upgrade → Sec-Websocket-Version →
   Sec-Websocket-Key`; `cspot` manda `Host → Upgrade → Connection →
   Sec-WebSocket-Key → Sec-WebSocket-Version → User-Agent`. RFC 6455 no
   exige un orden - HTTP en general tampoco - pero es una diferencia de
   bytes real entre ambos clientes.
2. **Casing distinto**: la librería HTTP de Go normaliza a
   `Sec-Websocket-Key`/`Sec-Websocket-Version` (su
   `http.CanonicalHeaderKey` solo capitaliza la letra después de cada
   guion, no sabe que "WebSocket" lleva una S mayúscula en el medio);
   `cspot` manda `Sec-WebSocket-Key`/`Sec-WebSocket-Version` (el casing
   "de libro" de la RFC). Nombres de header HTTP son case-insensitive
   por spec - no debería importarle a un servidor conforme - pero es
   otra diferencia de bytes real.
3. **La respuesta del servidor no distingue nada de esto**: `101
   Switching Protocols` en ambos casos, sin fricción - confirma (otra
   vez) que el handshake en sí nunca es donde aparece un problema; el
   patrón de cierre ya documentado (§22 en adelante) llega minutos
   después de una conexión ya establecida y funcionando, no en el
   handshake.

**Lectura honesta**: estas dos diferencias son reales y verificadas,
pero ninguna es candidata fuerte para explicar el cierre - HTTP no es
sensible a orden/casing de headers por especificación, y la respuesta
del servidor es idéntica (`101`) para ambos. Quedan como diferencias de
"fingerprint" documentadas, en el mismo espíritu que el fingerprint TLS
de §28 (que sigue siendo el candidato más plausible y el único que
requeriría una capa por debajo de HTTP - el `ClientHello` de TLS en sí -
para confirmar o descartar).

## Resultado real: 20+ minutos, la conexión nunca se cerró (2026-07-16 10:12)

El usuario dejó la sesión reproduciendo, con varios cambios de track
automáticos, un `skip_next` real (`09:56:19`) y, después de varios
minutos de idle, un `play` con cambio de contexto real (`10:12:09`,
cambio de playlist) - exactamente el patrón que en `cspot` dispara el
cierre de forma confiable desde §22.

```bash
grep -n "dealer connection closed\|connected to guc\|dealer recv loop stopped\|HANDSHAKE-DEBUG.*GET" golibrespot_debug.log
```

Resultado: **un solo handshake** (`09:52:38`, el de la conexión inicial)
y **ningún** `"dealer connection closed"`/`"dealer recv loop stopped"` en
los 20+ minutos siguientes. La conexión del Dealer de `go-librespot`
sigue abierta sin cortes atravesando el mismo patrón exacto (idle largo
+ comando que cambia de contexto) que siempre cierra la de `cspot`.

Esto reafirma con mucha más evidencia el A/B puntual de §27 - no es una
coincidencia de una sola prueba. Con el handshake HTTP ya comparado
(diferencias de orden/casing de headers, ninguna con mecanismo causal
plausible) y el ping/pong ya descartado (§31/§33/§35), lo único que
sigue en pie sin poder confirmarse desde el código es el **fingerprint
TLS del `ClientHello`** (cipher suites, extensiones, orden - mbedTLS en
el ESP32 vs `crypto/tls` de Go) - la capa que queda por debajo de todo
lo ya comparado, y la única para la que haría falta una captura de
paquetes real (tcpdump/mitmproxy) para confirmar o descartar
definitivamente.

## Captura de paquetes del `ClientHello` (2026-07-16, en curso)

`tcpdump` necesita privilegios elevados (`sudo`) que la sesión de Claude
no puede ejercer (pide contraseña interactiva, no hay forma de tipearla
desde la tool de Bash) - el usuario corre la captura directamente en su
propia terminal.

**Comando corrido por el usuario** (no por Claude):

```bash
sudo tcpdump -i any -w <scratch>/pcap/dealer_handshake.pcap \
    host guc3-dealer.spotify.com and tcp port 443
```

**Coordinación necesaria** (la conexión actual del Dealer ya estaba
establecida desde hace rato - captura un handshake TLS *nuevo* hace
falta forzar una reconexión):
1. Usuario arranca la captura de arriba en su propia terminal.
2. Claude reinicia `golibrespot_debug` (mismo binario con
   `handshakeLogger`, §anterior de este documento) para perder la
   conexión actual.
3. Usuario re-selecciona "cspot-verificacion" desde el picker de Connect
   del teléfono, disparando el emparejamiento Zeroconf + el handshake
   TLS/WS nuevo hacia `guc3-dealer.spotify.com:443`.
4. Usuario corta la captura (`Ctrl+C`) una vez completada la
   reconexión.
5. `sudo chmod 644 <pcap>` - el archivo queda dueño `root` (porque
   `tcpdump` corrió con `sudo`), hace falta abrirlo para que Claude
   pueda leerlo sin privilegios.

**Análisis pendiente**, una vez que el `.pcap` esté disponible: extraer
el mensaje `ClientHello` de la sesión TLS hacia el Dealer (cipher
suites ofrecidas, extensiones, versión de TLS, ALPN, orden) - sin
`tshark` instalado en este entorno, se va a parsear con Python
(`scapy`, instalado en un venv si hace falta) o a mano, leyendo la
estructura del record TLS crudo del `.pcap`. Comparar contra lo que
`mbedtls_ssl_config_defaults()` + `mbedtls_ssl_conf_authmode()` +
`mbedtls_ssl_setup()` arman en `TLSSocket::open()`
(`bell/main/io/TLSSocket.cpp`) - sin ALPN configurado, sin SNI más allá
de `mbedtls_ssl_set_hostname()`, defaults del preset
`MBEDTLS_SSL_PRESET_DEFAULT` para el resto (cipher suites, versión
mínima/máxima).

## `ClientHello` capturado de verdad - `go-librespot` (2026-07-16 10:35:38, TCP dport 443 hacia `guc3-dealer.spotify.com`)

```
legacy_version: 0303 (TLS 1.2, campo legacy - normal aun negociando 1.3)
cipher_suites (13):
  0xc02b ECDHE-ECDSA-AES128-GCM-SHA256
  0xc02f ECDHE-RSA-AES128-GCM-SHA256
  0xc02c ECDHE-ECDSA-AES256-GCM-SHA384
  0xc030 ECDHE-RSA-AES256-GCM-SHA384
  0xcca9 ECDHE-ECDSA-CHACHA20-POLY1305
  0xcca8 ECDHE-RSA-CHACHA20-POLY1305
  0xc009 / 0xc013 / 0xc00a / 0xc014  (variantes CBC-SHA, legacy)
  0x1301 TLS_AES_128_GCM_SHA256       (TLS 1.3)
  0x1302 TLS_AES_256_GCM_SHA384       (TLS 1.3)
  0x1303 TLS_CHACHA20_POLY1305_SHA256 (TLS 1.3)
compression: [0]  (ninguna)
extensions (11), en orden real:
  server_name -> guc3-dealer.spotify.com
  ec_point_formats
  renegotiation_info
  extended_master_secret
  signed_certificate_timestamp
  status_request
  supported_groups
  signature_algorithms
  signature_algorithms_cert
  supported_versions -> 0x0304, 0x0303  (TLS 1.3 preferido, 1.2 como fallback)
  key_share
```

**Sin extensión ALPN** - confirma lo que ya se sospechaba leyendo el
código: ni `go-librespot` ni `cspot` la ofrecen para esta conexión (Go
la omite a propósito para un upgrade WS, que necesita HTTP/1.1 puro;
`cspot` nunca llama a `mbedtls_ssl_conf_alpn_protocols()` pese a tener
`CONFIG_MBEDTLS_SSL_ALPN=y` compilado). Coincide en ambos - no es un
diferenciador real.

## Lo que `cspot` mandaría - inferido del `sdkconfig` real, no capturado

Sin hardware para capturar directamente, esto sale de leer la
configuración real de mbedTLS que usa el firmware (`sdkconfig`) contra
el código fuente real de mbedTLS (`ssl_ciphersuites.c`), mismo rigor que
el resto de esta investigación - no una suposición genérica.

**Hallazgo mayor, no cosmético**: `CONFIG_MBEDTLS_SSL_PROTO_TLS1_3 is
not set` en el `sdkconfig` actual - `cspot` negocia **solo TLS 1.2**, a
diferencia de `go-librespot`, que como se ve arriba prefiere y
normalmente consigue **TLS 1.3** contra los servidores de Spotify. Esto
ya se había probado (§20, motivado por latencia de handshake, no por
esta investigación) y se revirtió tras una falla real en hardware -
retomado en detalle en la próxima sección.

**Cipher suites**: con `MBEDTLS_KEY_EXCHANGE_RSA`/`ECDHE_RSA`/
`ECDHE_ECDSA` habilitados, `GCM`/`CCM` sí, `CHACHA20`/`CAMELLIA`/`ARIA`
**no** (confirmado en `sdkconfig`), el preset default de mbedTLS
(`ssl_ciphersuites.c`, filtrado contra lo compilado) ofrecería del orden
de **15-20+ cipher suites** TLS 1.2 - incluyendo variantes CBC-SHA
"legacy" (no solo GCM) y, dado que `MBEDTLS_KEY_EXCHANGE_RSA=y` también
está activo, hasta suites RSA puras (sin forward secrecy). Contra los
**13** de `go-librespot` (10 ECDHE-AEAD TLS 1.2 + 3 TLS 1.3, sin CBC
legacy, sin RSA puro - filosofía de Go: lista corta, moderna, sin nada
heredado). Es una diferencia de "fingerprint" real y bastante marcada -
un cliente que ofrece una lista larga e inclusiva de cifrados legacy es
justamente el tipo de señal que sistemas de fingerprinting tipo JA3
usan para distinguir un stack TLS embebido/genérico de un cliente
moderno curado - especulativo como causa del cierre, pero un
diferenciador real y de peso, no menor como el orden/casing de headers
HTTP.

**Extensiones esperables igual en ambos**: `server_name` (SNI, ambos lo
mandan), `renegotiation_info` (`CONFIG_MBEDTLS_SSL_RENEGOTIATION=y` en
`cspot` - coincide con que Go también la manda). `signed_certificate_
timestamp`/`status_request` son extras que el stack de Go agrega por
default y mbedTLS no manda salvo configuración explícita (no hecha acá)
- presentes en `go-librespot`, ausentes en `cspot`. `key_share`/
`supported_versions` son exclusivas de TLS 1.3 - no aplican a `cspot`
mientras esté en solo-1.2.

## Corrección importante: `go-librespot` sí se desconectó, pero por idle puro, no por comando (2026-07-16 10:44:04)

Mientras se armaba la comparación de arriba, la conexión capturada en el
handshake de `10:35:38` se cayó de verdad:

```
10:35:41  fetched chunk 1/11 ... (última actividad visible en el log)
                                    <- 8.4 minutos de silencio total
10:44:04  error="failed to get reader: failed to read frame header: EOF"
10:44:04  reconecta solo a gae2-dealer.spotify.com (antes: guc3)
```

**Diferencia clave con el patrón de `cspot`**: acá no hubo ningún comando
justo antes del corte - fue idle puro, sin ninguna acción del usuario en
el teléfono. `cspot` en cambio nunca se cierra en medio de un idle puro
(confirmado repetidas veces en esta investigación: el ping/pong sigue
funcionando bien durante minutos) - se cierra siempre a milisegundos de
contestar un comando que cambia de contexto, nunca antes de que llegue
uno.

**Lectura**: probablemente son dos fenómenos distintos, no el mismo:
1. **Reciclado genérico por idle** (posiblemente universal del backend
   de Spotify, sin relación con el cliente) - esto es lo que le acaba de
   pasar a `go-librespot`, tratado como una reconexión más sin
   sospechar nada, exactamente como ya se leyó en su código (§22-27):
   `dealer/dealer.go`'s `recvLoop()` no distingue esta causa de
   cualquier otra.
2. **El patrón específico y determinístico de `cspot`**: cierre a
   milisegundos de un comando que cambia de contexto, nunca en idle
   puro - esto `go-librespot` no lo mostró ni una sola vez en los 20+
   minutos observados, con varios comandos reales de por medio
   (`skip_next`, `transfer`, `play` con cambio de playlist).

La conclusión de la sección anterior no cambia (el patrón 2 sigue sin
aparecer en la referencia) - se corrige únicamente la afirmación de que
"la conexión nunca se cierra": sí se cierra, por una causa genérica de
idle que probablemente comparten ambos clientes y que ninguno de los
dos puede evitar - la pregunta abierta sigue siendo por qué **cspot**
además sufre el patrón 2, que la referencia nunca mostró.
