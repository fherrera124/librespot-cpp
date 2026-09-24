# MitM Windows: antecedentes y límites de reproducción

Estas son **notas históricas**, no el estado de un proxy activo ni un requisito
para continuar los controles guardados. En la sesión2026-09-24 se capturaron
respuestas dentro de Spotify148 con Frida; no se capturó por MitM su
token/version de request. El siguiente paso usa fixtures y está en
[PLAN.md](../PLAN.md).

## Qué hacían los archivos

`dump_playplay.py` es un addon de mitmproxy. En el hook `request`, si la URL
contiene `playplay/v1/key`, escribe **solo el body** a
`C:\Users\francisco.herrera\playplay_dump.bin`, modo `wb`.
Cada request sobrescribe el anterior. No conserva headers, URL/file_id,
timestamp, response ni múltiples capturas. No es una captura correlacionada
suficiente por sí sola; inspeccionarlo antes de reutilizar.

Se documentó históricamente instalación de mitmproxy y su CA en Windows.
No se revalidó en esta consolidación que sigan instalados, confiados o activos.
Tampoco se revalidaron entradas hosts, listeners ni IP upstream.

## Intentos registrados

Los ensayos antiguos con preferencias `network.proxy.*`, argumento
`--proxy-server` y proxy del sistema no consiguieron interceptar la ruta usada;
uno dejó el cliente sin red. Esos resultados pertenecen al entorno ensayado:
no son demostración de que todos los clientes/builds evadan cualquier proxy.

Se probaron dos disposiciones que las notas anteriores mezclaban:

| Disposición histórica | Qué requiere / límite |
|---|---|
| `mitmdump -p 8086 -s dump_playplay.py` | Proxy normal. Escuchar en8086 **no redirige por sí solo** el HTTPS del cliente que lo ignora |
| Hosts a127.0.0.1 + reverse proxy443 | Dirigir el hostname utilizado al listener TLS local, con upstream real independiente de esa resolución |

La configuración reverse documentada apuntaba
`spclient.wg.spotify.com` en hosts a127.0.0.1 y utilizaba como upstream una IP
observada entonces (35.186.224.24). **No reutilizar esa IP como dato actual**.
El comando histórico era:

```text
mitmdump -k --mode reverse:https://35.186.224.24 -p 443 --set keep_host_header=true -s dump_playplay.py
```

Se guarda para explicar el ensayo, no como instrucción vigente de despliegue.
`-k` omitía validación TLS upstream para ese experimento; no es un requisito
demostrado de una solución definitiva. Las notas anteriores llamaban a esta
vía «única solución»: esa exclusividad no está probada.

## Si se necesita reabrir esta vía

Primero definir qué dato falta: token/version del request Windows, campo de
respuesta o asociación recurso/contenido. La captura Frida actual ya registra
recurso, respuesta y callback sin proxy; no repetir MitM para obtener lo mismo.

Revisar código del addon y versión instalada, hostname realmente usado,
resolución upstream fuera de cualquier override, puerto443 disponible y CA
del entorno. Registrar/respaldar configuración previa para revertir **solo los
cambios propios**. No dejar entradas hosts apuntando a un listener detenido.

Antes de prometer una captura, comprobar un evento real y guardar body con
su recurso y dirección request/response en archivos distintos, sin sobrescribir
los anteriores. El body protobuf permitiría leer token/version si ese request
se captura, pero las notas conservadas no certifican ese resultado para148.

No se modificó proxy, certificados, hosts ni addon durante este cierre
documental. Estado experimental final y evidencias:
[VALIDATION_148_2026-09-24.md](../docs/VALIDATION_148_2026-09-24.md).
