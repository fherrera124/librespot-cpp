# Token148/v5: prueba puntual por SSH, sin servicio

Explicación para principiantes y próximos experimentos:
[mecanismo y extracción AES](MECANISMO_Y_EXTRACCION_AES.md).

Este informe conserva el procedimiento y resultado de la corrida original.
Después se recuperaron ambas AES por DFA y se validó la API; ver
[hito DFA](../dfa_attack_results.md) y [STATUS](../STATUS.md). El ground truth
vigente es [schema2](../data/ground-truth-vectors.json). Las fuentes exactas de
esta corrida permanecen en [source](../runs/20260924T164814Z-token148/source/).

## Resultado

**Control de contenido positivo** con token `02d29f82a8396930aab0a5885c81da7a`,
protocolo 5 y Spotify Windows 1.2.92.148. Para dos recursos con AES publicada,
el generador nativo produce exactamente 4096 bytes de AES-128-CTR con el IV
estándar. El XOR con el prefijo CDN produce el mismo contenido que la AES de
referencia y una primera página Ogg/Vorbis con CRC válido.

Se probaron `b4_seq` capturado y cuatro bytes cero, dos ejecuciones por variante:
2 recursos × 2 auxiliares × 2 ejecuciones × 256 bloques. **4/4 casos pasan.**
No se reprodujo una pista completa ni se extrajo una AES nueva: las AES de
referencia ya estaban en los fixtures. El candidato de `0x49f854` sigue sin ser
esa AES y falla el control de contenido en ambos recursos.

Esto contradice la conclusión anterior de que el cliente 148 necesariamente
usa un cifrado de contenido incompatible con AES-128. No determina todavía
la representación interna de sus tablas ni el algoritmo de extracción de clave.

## Procedimiento y evidencia

Identificador de corrida: **20260924T164814Z**. Las fechas de cada petición están
en los JSON. No se inició servidor HTTP ni se modificó C++.

1. [Preflight](token148-preflight-20260924T164814Z.json): PID principal 72132,
   build 1.2.92.148, SHA256 de disco
   `7b44456a90142daeb758e2736d8628ffe211d6ea523db8f1050b3c8b1addb68a`.
2. [Control anterior](token148-baseline-20260924T164814Z.json): 13 casos × 2,
   cuatro bloques repetibles; sin coincidencia con las AES/IV del fixture.
3. Autenticación local desde `session.json` siguiendo `CredentialsResolver.cpp`;
   bearer/client-token solo en memoria. No se copiaron credenciales a Windows
   ni se guardaron en los informes.
4. Dos licencias token148/v5, interactividad1, contenido1 y timestamp actual:
   HTTP200. Se conservan request/response, `obfuscated_key`, `b4_seq`, `file_id`,
   AES de referencia y hash del contenido. Storage-resolve HTTP200 y CDN HTTP206,
   rango `0-4095`. No se guardaron URLs firmadas ni headers de autenticación.
5. Runtime fresco `0x4a0268`, descriptor28, inicializador `0xd9e2e4` y generador
   `0xd9d0f0`. Primero cuatro bloques, luego 256. Callback del objeto local
   deshabilitado. `b4_seq` copiado al tercer argumento por sus bytes, sin
   conversión endianness. Su significado global sigue abierto.
6. [Verificación independiente](token148-content-verification-20260924T164814Z.json):
   comparación completa AES-128-CTR, repetición, contenido, Vorbis y CRC Ogg.

| Recurso | Licencia | Prefijo cifrado |
|---|---|---|
| `2f43127d80edc9cd9f12f441e1cb7904b680f9da` | [JSON](token148-license-20260924T164814Z-retry.json) | [4096 B](../data/token148-reference-20260924T164814Z.enc) |
| `1a8e5b04837957617162724232b0c96922222447` | [JSON](token148-license-second-20260924T164814Z.json) | [4096 B](../data/token148-reference-second-20260924T164814Z.enc) |

Informes VM: [cuatro bloques](token148-fresh-vm-20260924T164814Z.json),
[256 bloques](token148-content-vm-20260924T164814Z.json).
Fixtures: [inicial](../data/token148-fresh-controls-20260924T164814Z.json),
[contenido](../data/token148-content-controls-20260924T164814Z.json).

## Repetir offline

Desde la raíz del repositorio, con `cryptography`, elegir informe nuevo:

```sh
python3 research/playplay/tools/verify_token148_content.py \
  --vm research/playplay/docs/token148-content-vm-20260924T164814Z.json \
  --license research/playplay/docs/token148-license-20260924T164814Z-retry.json \
  --license research/playplay/docs/token148-license-second-20260924T164814Z.json \
  --report /tmp/token148-verificacion-nueva.json
```

No requiere Spotify, red ni credenciales. Devuelve 0 solo si todos los casos
incluidos pasan. Compara todos los bytes del stream, no solo la cabecera.

## Repetir con Windows

Leer primero los scripts. `probe_token148_once.py` corre **localmente en Linux**:

```sh
python3 research/playplay/tools/probe_token148_once.py \
  --session session.json --report /tmp/token148-licencia-nueva.json \
  --content /tmp/token148-prefijo-nuevo.enc
```

Solicita el primer recurso de la tabla por defecto. Para otro, proporcionar
juntos `--file-id` y `--aes`. `--interactivity 3` permite contrastar la captura
Windows; los éxitos documentados usan 1, igual que cspot. No sobrescribe salidas.

El runner actual utiliza exclusivamente los dos casos del ground truth schema2.
Copiar a Windows `tools/validate_windows_vm.py`, `tools/playplay_148_preflight.py`,
`tools/check_fresh_license_148.js` y `data/ground-truth-vectors.json`, conservando
la estructura tools/data. Frida y psutil se usan solo para captura; las fuentes
de evidencia y `cryptography` se requieren para evaluar en Linux.

Desde `tools` en Windows, con PID del proceso principal recién verificado:

```powershell
py -3 .\validate_windows_vm.py --pid <PID_VERIFICADO> `
  --vectors ..\data\ground-truth-vectors.json --capture-only --report contenido-nuevo.json
```

El preflight comprueba build, hash y firmas antes de instalar hooks. La captura
predeterminada inyecta obfuscated_key/b4_seq y genera 4096 B dos veces por caso;
las AES de referencia se usan al evaluar offline. Copiar el informe a Linux:

```sh
python3 research/playplay/tools/validate_windows_vm.py --evaluate-report /tmp/contenido-nuevo.json
```

Los modos `--script`, `--script-data`, `--exceptions` y `--advance-track` siguen
disponibles para los [diagnósticos148](../tools/README.md); no certifican por sí
solos coincidencia criptográfica. El runner original devolvía código2 para los
eventos alternativos de esta corrida. Sus informes se verifican con el comando
`verify_token148_content.py` de la sección anterior, no con `--evaluate-report`,
que exige el formato y hash del fixture actuales.

## Incidencias y límites

- [Primer intento local](token148-license-20260924T164814Z.json): parser diagnóstico
  rechazaba campos protobuf repetidos. Corregido para admitirlos (p. ej., dominios
  del client-token); el segundo intento autenticó. No fue rechazo de credenciales.
- `node` no está instalado localmente. El JS se cargó y ejecutó con Frida;
  no se presenta un chequeo Node como realizado.
- El usuario restauró la nota externa durante la sesión. Su dump tiene campo4=3
  y campo5=1: según el schema local, interactividad y contenido respectivamente.
  El schema local no define interactividad3.
- No se volvió a consultar E al backend; no se afirma que dejara de aceptarlo.
- Al cierre de esta corrida quedaban pendientes la extracción AES16 y el
  funcionamiento autónomo; posteriormente se logró DFA/API con dos controles.
  Siguen pendientes licencia fresca, pista completa, arranque frío offline y E2E ESP32. Seguir relacionando la representación interna con
  las **dos AES confirmadas por contenido**, manteniendo este control positivo.

## Cierre y controles del código

[Postflight](token148-postflight-20260924T164814Z.json): a las 17:00 UTC, PID72132
responde, no quedan runners Python del ensayo y no hay listener8080. Solo aparece
el Python preexistente de VS Code. Los fixtures/scripts remotos permanecen en la
carpeta aislada para reproducibilidad; no hay servicio ejecutándose.

Sintaxis Python comprobada. El SHA256 del JS original conservado en source, compuesto con los fixtures
coincide exactamente con `script_sha256` del informe de 256 bloques. Comprobados
parser protobuf con campos repetidos y rechazo de truncamientos; el validador Ogg
acepta el contenido de referencia y rechaza un byte alterado y el ciphertext.
`git diff --check` pasa. No se compila C++ porque esta sesión no lo modificó.
