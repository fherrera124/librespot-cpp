# Reglas y Normas de Trabajo: PlayPlay LAN Deobfuscator

Este proyecto trata de realizar un *Man-in-the-Middle* (MitM) o ingeniería inversa sobre el cliente de Spotify para extraer llaves de audio de PlayPlay.

Si eres un agente (o subagente) trabajando en esta carpeta, **debes seguir estrictamente estas reglas**:

## 1. Obligaciones Previas a la Ejecución
*   **Lectura Obligatoria:** Lee SIEMPRE `PLAN.md` (en la raíz) y el `task.md` para entender el estado actual del proyecto antes de escribir código o ejecutar comandos en el sistema.
*   **Revisión Histórica:** Lee `docs/WINDOWS_WORK_SUMMARY.md` y `docs/FACTS.md` para entender por qué ciertas decisiones arquitectónicas ya se han tomado.

## 2. Ejecución y Pruebas
*   **NO ejecución ciega:** No ejecutes scripts de la carpeta `tools/` o `archive/` sin antes inspeccionar su código. Muchos de estos scripts fueron pruebas descartadas o asumen contextos (como paths específicos o versiones de DLLs) que ya no aplican.
*   **PC Windows Remota:** El entorno principal de pruebas es un host Windows accesible vía SSH (ver `research/acceso-ssh-pc-windows.md`).
    *   Usa estrictamente la clave privada documentada (`~/.ssh/win_claude`).
    *   Si realizas pruebas que dejen procesos colgados, recuerda usar PowerShell para limpiar `Spotify.exe`.
*   **MitM Proxy:** Si necesitas interceptar tráfico en Windows, revisa `resources/mitm_setup_notes.md` ya que el cliente de Windows evade configuraciones de proxy habituales, forzándonos a usar DNS spoofing y un reverse proxy.

## 3. Registro y Documentación
*   **Actualiza el Checklist:** Usa `task.md` para marcar tu progreso `[x]` o `[/]`.
*   **Bitácora:** Todo avance significativo, solución a bloqueos, o cambio de dirección debe documentarse al final de tu sesión en `docs/WINDOWS_WORK_SUMMARY.md` u otro documento de bitácora relevante.
*   **Organización:** Cualquier script nuevo que crees debe ser depositado en `tools/` si es de análisis, o en `resources/` si es un script listo para el entorno Windows final. NO dejes scripts sueltos en la raíz.

## 4. Trabajo Multi-Agente
*   Si eres un agente delegando tareas a un **subagente**, DEBES pasarle explícitamente el contexto (ej. indicarle que lea `PLAN.md` y estas reglas).

