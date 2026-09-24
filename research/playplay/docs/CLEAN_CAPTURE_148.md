# Capturas limpias 148: procedencia y controles

## Fuentes recuperadas

Los dos scripts recuperados coincidían con stage. A cada uno le faltaba un LF
final: añadir exactamente ese byte reprodujo el SHA256 registrado en su informe.
Antes de modificar las herramientas se conservaron esas fuentes en
[runs/20260924-clean-capture-recovery](../runs/20260924-clean-capture-recovery/manifest.json).
Esto es recuperación offline de fuentes históricas, no una captura nueva Windows.

| Capturador | SHA256 de la fuente histórica restaurada | Evidencia |
|---|---|---|
| `capture_clean_148.js` | `fa372ebdf73d71d20e4dd27c67d9f04ef3ed457b271c94e38f675f4058773f93` | [Informe](clean_capture_report_2026-09-24.json) |
| `capture_context_148.js` | `c7b0b4fddbe464d094eec1ad7134572f497f004812b6f5bf6c53b435387987db` | [Informe](context_dump_2026-09-24.json) |

Ambos informes terminan con `clean_capture_done`. El segundo contiene dos pares
de estados de 740 B y descriptores de 28 B. Sus primeros bloques coinciden con
AES128 aplicada al IV de referencia para cada caso; esa comparación se hace
fuera del proceso instrumentado. Que estos scripts no inserten claves conocidas
no garantiza ausencia de contaminación de ensayos anteriores en el mismo proceso.

## Herramientas actuales y uso

[capture_clean_148.js](../tools/capture_clean_148.js) observa el pipeline para dos
entradas fijas. [capture_context_148.js](../tools/capture_context_148.js) además
inicializa un contexto privado y genera un bloque. **No son el solver DFA.**
Los hooks filtran por hilo propio. El segundo libera su hook dentro de `finally`,
incluso cuando la llamada nativa arroja una excepción que llega a JavaScript.
Esto no garantiza recuperación de un crash nativo no manejado.

Ejecutar mediante [run_clean_capture.py](../tools/run_clean_capture.py), con
[playplay_148_preflight.py](../tools/playplay_148_preflight.py) en la misma carpeta.
Requiere Windows, Python 3.11+, Frida y psutil. El runner:

1. Comprueba que el PID seleccionado corresponde al Spotify.exe principal.
2. Adjunta una sonda de metadatos sin hooks ni llamadas a RVA de Spotify y obtiene
   la ruta/base/tamaño del módulo cargado. Exige Windows x64.
3. Lee la versión fija de esa DLL con `version.dll`; exige `1.2.92.148` y SHA256
   `7b44456a90142daeb758e2736d8628ffe211d6ea523db8f1050b3c8b1addb68a`.
4. Inyecta un guard que vuelve a comprobar identidad del módulo y los primeros
   16 bytes de las cinco entradas utilizadas, antes de hooks o NativeFunction.
5. Registra identidad, hash de fuente y hash del código ejecutado con el guard.
   Devuelve código 1 ante error, timeout, interrupción o finalización incompleta;
   siempre intenta liberar script y sesión. Código 0 indica captura terminada,
   no verificación criptográfica de sus resultados.

El hash corresponde al **archivo en disco**, no a toda la imagen en memoria.
Las firmas contrastan puntos concretos del código cargado. Este control detecta
un build distinto o entradas alteradas; no certifica toda la memoria del proceso.
Los JS actuales rechazan ejecución directa sin el guard del runner. No copiar un
perfil 148 a otra versión ni eludirlo cambiando solamente su versión esperada.

Desde PowerShell, en la carpeta que contiene los tres archivos necesarios:

```powershell
# Definir $ppPid con el PID principal recién verificado; no reutilizar uno histórico.
py -3 .\run_clean_capture.py --pid $ppPid --script .\capture_context_148.js --report .\context-nuevo.json --timeout 30
```

Para la otra captura cambiar `--script` y usar otro nombre de informe. No requiere
servicio LAN ni cambiar de canción: ejecuta las dos entradas de prueba fijadas en
el script, sobre un Spotify ya inicializado. Para preservar evidencia guardar
runner, helper y JS exactos junto con el informe en una nueva carpeta `runs/`.
Mantener AES de referencia y credenciales de cuenta fuera de esta captura.

## Comprobaciones de esta revisión

[test_clean_capture.py](../tools/test_clean_capture.py) prueba offline el rechazo
de identidad/versión/hash, la no carga del capturador si falla preflight, limpieza
de sesión, errores y timeout. Con el módulo opcional `quickjs` también ejecuta
los guards, comprueba sintaxis de ambos JS y simula una excepción del pipeline
para comprobar que se libera el hook. Sin quickjs esos tres tests se omiten.

```sh
python3 research/playplay/tools/test_clean_capture.py
python3 research/playplay/tools/check_workspace.py
```

Los dobles de prueba no ejecutan `version.dll`, Frida ni Spotify reales. La nueva
versión de las herramientas requiere una corrida Windows para cerrar su
validación de integración. Las capturas históricas prueban las fuentes archivadas,
no la ejecución del guard incorporado después.

Para derivación histórica de RVA y procedimiento para nuevas versiones, consultar
[RVA_DISCOVERY_PLAYBOOK](RVA_DISCOVERY_PLAYBOOK.md).
