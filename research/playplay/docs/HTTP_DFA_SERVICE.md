# Servicio HTTP de AES para cspot

Versión del repositorio: 2026-09-25. Implementación:
[servidor](../tools/playplay_dfa_service.py), [RPC Frida](../tools/playplay_dfa_rpc.js).
Windows ejecuta la transformación nativa y el DFA; cspot recibe la AES de
contenido de 16 bytes. Spotify debe estar abierto en la sesión gráfica.
Build admitido: 1.2.92.148, con el SHA256 de disco del
[preflight](../tools/playplay_148_preflight.py).

## Contrato

`POST /deob`, `Content-Type: application/json`:

```json
{"obfuscated_key":"<32 caracteres hex>","b4_seq":"<8 caracteres hex>"}
```

Ambos campos son obligatorios y se envían como vienen en la licencia. No se
envían bearer, Client-Token, sesión de cuenta ni AES de referencia a Windows.

Respuesta HTTP 200:

```json
{"success":true,"aes_key":"<32 caracteres hex>"}
```

Antes de responder, el solver recupera K10, invierte el key schedule y verifica
AES(K0, IV de audio) contra el bloque nativo. No utiliza tablas de claves
conocidas. Respuestas con `Cache-Control: no-store`; no registra claves.

| Estado | Significado |
|---|---|
| 400 | Campos ausentes, longitud incorrecta o hex inválido |
| 401 | Token de servicio ausente/incorrecto cuando está configurado |
| 413 / 415 | Cuerpo mayor de 1024 bytes / formato distinto de JSON |
| 503 `busy` | Extracción en curso; no se encolan solicitudes |
| 503 `extraction_failed` | Worker no disponible o extracción no validada |
| 504 `extraction_timeout` | Límite de 20 segundos agotado; worker deshabilitado |

`GET /health`: `ready`, `busy`, última etapa (`stage`), PID y build/hash
verificados. `ready` indica disponibilidad del worker, no validación de una
licencia particular. También exige el token si está configurado.

El worker es un proceso separado que contiene la sesión Frida. Al exceder el
plazo se termina ese worker, nunca Spotify. Esto acota la espera HTTP; no
garantiza cancelar una instrucción nativa que estuviera ejecutándose dentro
de Spotify. Tras un timeout, revisar el cliente y reiniciar el servicio.
No hay reintento ni reenganche automáticos.

## Instalación Windows

Copiar a una carpeta estos archivos de `research/playplay/tools`:

- `playplay_dfa_service.py`, `playplay_dfa_rpc.js`, `playplay_148_preflight.py`.
- `extractor.py`, `reverse_key_schedule.py`.
- `playplay_service_requirements.txt`, `start_playplay_service.ps1`,
  `stop_playplay_service.ps1`, `install_playplay_service.ps1`.

Python 3.11 o posterior. En PowerShell, desde esa carpeta:

```powershell
$ppPython = "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe"
& $ppPython -m pip install -r .\playplay_service_requirements.txt
.\start_playplay_service.ps1
```

El launcher exige exactamente un PID principal actual; no arranca ni cierra
Spotify. Escucha en `127.0.0.1:8765`. Para independizarlo de SSH, detener primero
el servidor en primer plano y ejecutar, con Spotify abierto:

```powershell
.\install_playplay_service.ps1
Get-ScheduledTask -TaskName CspotPlayPlayDfa
```

Crea e inicia una tarea **a demanda** en la sesión del usuario conectado.
No almacena contraseña ni agrega reglas de firewall. Rechaza sobrescribir
una tarea existente. Después de reiniciar Windows o Spotify:

```powershell
.\stop_playplay_service.ps1
Start-ScheduledTask -TaskName CspotPlayPlayDfa
```

Al iniciar detecta el nuevo PID. Para retirar la tarea:

```powershell
.\stop_playplay_service.ps1
Unregister-ScheduledTask -TaskName CspotPlayPlayDfa -Confirm:$false
```

La parada ordenada descarga el script Frida y se desadjunta antes de salir.
La tarea ejecuta Python directamente y reserva el puerto de forma exclusiva
antes de adjuntar; no debe haber dos instancias sobre el mismo Spotify.
La corrida inicial encontró procesos residuales al detener una tarea con un
wrapper PowerShell y un cierre con fallo en `frida-agent.dll`; se conserva ese
resultado negativo. No usar la terminación forzada como procedimiento normal.

## Conexión desde cspot CLI en Linux

Con la VPN conectada, mantener este túnel en ejecución:

```sh
ssh -N -i ~/.ssh/win_claude -o ExitOnForwardFailure=yes \
  -o ServerAliveInterval=30 \
  -L 127.0.0.1:8765:127.0.0.1:8765 \
  francisco.herrera@10.16.150.154
```

En otra terminal:

```sh
curl --noproxy '*' http://127.0.0.1:8765/health
PLAYPLAY_SERVICE_URL=http://127.0.0.1:8765/deob ./build/targets/cli/cspot_cli
```

La URL es el nuevo valor por defecto del cliente. Timeout de I/O: 25 segundos.
Se conserva el modo PlayPlay forzado de este árbol. El cliente valida JSON,
`success=true` y exactamente 16 bytes hex; conserva `b4_seq` al decodificar
la licencia. Se retiró el volcado de credenciales a `/tmp/creds.json`; no se
borraron archivos preexistentes de otras sesiones.

Para usar directamente una interfaz privada, el servidor exige
`PLAYPLAY_SERVICE_TOKEN` y `--host <IP>`. cspot envía la misma variable en
`X-PlayPlay-Token`. Ese modo y ESP32 no se probaron en esta corrida; el túnel
SSH local es la configuración comprobada. No se abrieron endpoints públicos.

## Verificación y alcance

[Corrida HTTP](../runs/20260925-http-dfa-service/EXPERIMENT.md): dos licencias
guardadas, dos extracciones por licencia, comparación de AES y prefijos de
4096 bytes con Ogg/Vorbis y CRC válidos. El evaluador corre en Linux; mantiene
las claves de referencia fuera de Windows.

```sh
python3 research/playplay/tools/verify_playplay_service.py \
  --license research/playplay/docs/token148-license-20260924T164814Z-retry.json \
  --license research/playplay/docs/token148-license-second-20260924T164814Z.json \
  --report /tmp/playplay-http-verificacion-nueva.json
```

El informe debe ser un archivo nuevo. Se conservan fuentes exactas y hashes
de Windows con la corrida. La prueba acredita extracción HTTP con esas licencias,
no reproducción completa de cspot, otro build ni arranque sin sesión.
Las pruebas Python cubren errores HTTP, serialización, timeout de worker y
recuperación offline; las pruebas C++ cubren petición y parser de AES.
