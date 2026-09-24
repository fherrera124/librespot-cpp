# Investigación de audio keys para cspot

## Estado vigente (2026-09-24)

La vía activa es [PlayPlay](playplay/README.md): el request E/v5 está documentado
con HTTP 200 en hardware. Se ejecuta la VM dentro de Spotify Windows 1.2.92.148
y se reprodujo un bloque de descifrado natural, pero **la extracción AES y el
E2E PlayPlay todavía no están validados**. Empezar por ese README y su PLAN.

Los binarios están en [dlls/](dlls/); scripts e informes de esta sesión en
[playplay/tools/](playplay/tools/README.md) y [playplay/docs/](playplay/docs/WINDOWS_WORK_SUMMARY.md).
`playplay-lan-deobfuscator/` conserva instrucciones históricas, no la implementación.
`tools/` contiene copias antiguas: revisar rutas antes de reutilizarlas.
La carpeta tiene archivos staged y no debe tratarse como material automáticamente
ignorado por Git.

## Vía partner/eSDK — investigación previa cerrada para este objetivo

La observación inicial de que el Sangean reproducía la cuenta afectada fue
corregida: el teléfono mostraba reproducción, pero no salía audio. Las pruebas
posteriores con el mismo eSDK/harness y cuentas distintas documentan audio para
la cuenta habilitada y silencio para la afectada. No se continúa buscando una
credencial partner como solución de cspot.

El [README del eSDK](esdk-emulation/README.md) conserva harness, resultados y
condiciones de ejecución. Son antecedentes; no se repitieron en esta sesión.
Acceso remoto documentado en [acceso-ssh-pc-windows.md](acceso-ssh-pc-windows.md).
