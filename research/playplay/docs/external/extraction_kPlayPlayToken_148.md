# Extracción del kPlayPlayToken en Spotify (Versión 1.2.92.148)

Este documento detalla el procedimiento técnico exacto utilizado para extraer el `kPlayPlayToken` actualizado desde el cliente oficial de Spotify para Windows (versión 1.2.92.148). Este token se usa durante las peticiones `playplay/v1/key` para obtener las licencias/llaves AES de las pistas.

## Contexto y Problema

El proyecto `librespot-cpp` enviaba en su código (ej. `SpClient.cpp`) un token hardcodeado conocido que comenzaba con `0x01, 0xF6, 0x2E, 0x56...`. Las peticiones para descifrar contenido moderno fallaban. En lugar de hacer ingeniería inversa exhaustiva de cifrado (MITM / enganches complejos a `libcef.dll`), se optó por una estrategia de **búsqueda en memoria viva** basada en el protocolo Protobuf usado por Spotify.

La estructura del payload en Protobuf de la petición `PlayPlayLicenseRequest` es conocida:
```protobuf
message PlayPlayLicenseRequest {
  int32 version = 1;      // Spotify v5 PlayPlay usa el valor 5 (hex: 08 05)
  bytes token = 2;        // Arreglo de 16 bytes (hex: 12 10)
  // Otros campos (Interactivity, ContentType, etc.)
}
```

## Estrategia de Búsqueda

Dado que buscar la firma completa o interceptar los sockets TLS (`libcef.dll` -> `WSASend`) da como resultado datos ya cifrados, la solución consistió en escanear la memoria de lectura/escritura (`rw-`) del proceso `Spotify.exe` **antes de que los datos sean pasados a BoringSSL para cifrarse**.

Se generó un escáner con **Frida** para buscar la secuencia de bytes correspondiente al encabezado del Protobuf:
- `08 05` -> Campo 1, tipo entero, valor 5.
- `12 10` -> Campo 2, tipo bytes, longitud 16 (0x10).

El patrón hexadecimal buscado fue: `08 05 12 10`.

## Scripts Utilizados

Se crearon dos archivos que se copiaron a la máquina destino mediante SSH/SCP para ejecutar el escaneo:

### 1. `scan_proto2.js` (Script de Frida)
```javascript
const pattern = "08 05 12 10"; // version 5 + token (16 bytes)
function toHex(buffer) {
    return Array.from(new Uint8Array(buffer), byte => byte.toString(16).padStart(2, '0')).join('');
}
let ranges = Process.enumerateRanges('rw-');
for (let i = 0; i < ranges.length; i++) {
    try {
        let matches = Memory.scanSync(ranges[i].base, ranges[i].size, pattern);
        for (let m of matches) {
            // Leer los siguientes 32 bytes después de la coincidencia
            let dump = m.address.readByteArray(32);
            let h = toHex(dump);
            send({ type: 'dump', data: h, address: m.address.toString() });
        }
    } catch(e) {}
}
send({ type: 'done' });
```

### 2. `run_scan_proto2.py` (Lanzador Python)
Este script de Python localiza automáticamente el proceso de Spotify, inyecta el script de Frida y escucha los resultados durante 5 segundos:
```python
import frida, psutil, time, sys

def on_message(message, data):
    if message['type'] == 'send':
        payload = message['payload']
        if payload.get('type') == 'dump':
            print(f"Match at {payload['address']}: {payload['data']}")
    else: print(message)

with open('scan_proto2.js', 'r') as f: js = f.read()
sessions = []
for p in psutil.process_iter(['pid', 'name']):
    if p.info['name'] and p.info['name'].lower() == 'spotify.exe':
        try:
            s = frida.attach(p.info['pid'])
            script = s.create_script(js)
            script.on('message', on_message)
            script.load()
            sessions.append(s)
        except: pass

time.sleep(5)
```

## Ejecución y Resultado

El usuario forzó la reproducción de una pista inédita manualmente para que se alojara la petición PlayPlay en memoria. Se ejecutó el lanzador Python mediante:
```bash
python run_scan_proto2.py
```

El volcado de memoria de las coincidencias arrojó el siguiente bloque (entre otros):
```
Match at 0x2583d0673d0: 0805121002d29f82a8396930aab0a5885c81da7a20032801309f8ad5d5060000
```

### Análisis del Protobuf Encontrado
Parseando los bytes localizados:
1. `08 05`: version = 5
2. `12 10`: token = bytes[16]
3. **`02 D2 9F 82 A8 39 69 30 AA B0 A5 88 5C 81 DA 7A`**: El **nuevo kPlayPlayToken**.
4. `20 03`: Campo 4 (ContentType?) = 3.
5. `28 01`: Campo 5 (Interactivity?) = 1.

*(Nota: En algunas ejecuciones también se encontró el string "1.2.92.148" empacado de manera similar cerca de un encabezado idéntico, lo cual ayudó a confirmar que estábamos en la zona de memoria correcta construyendo metadata del cliente).*

## Conclusión
El token fue rotado por Spotify. Para replicar la conexión en código C++, el token debe ser actualizado a la versión encontrada:
```cpp
constexpr std::array<uint8_t, 16> kPlayPlayToken = {
    0x02, 0xD2, 0x9F, 0x82, 0xA8, 0x39, 0x69, 0x30, 
    0xAA, 0xB0, 0xA5, 0x88, 0x5C, 0x81, 0xDA, 0x7A
};
```

Este procedimiento puede ser replicado íntegramente por cualquier agente copiando el código Python/JS y aplicando el mismo trigger manual durante la reproducción de una nueva pista.
