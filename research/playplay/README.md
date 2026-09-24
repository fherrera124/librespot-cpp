# PlayPlay LAN deobfuscator — punto de entrada

Estado consolidado al **2026-09-24**, después de las pruebas reales en Windows.
La carpeta operativa es `research/playplay/`; `research/playplay-lan-deobfuscator/`
solo contiene instrucciones históricas.

## Objetivo y resultado actual

cspot obtiene una `obfuscated_key` de PlayPlay con token E/version 5, pero todavía
no dispone de una **AES de contenido validada** para reproducir por ese camino.
La investigación usa Frida dentro del Spotify oficial de Windows **1.2.92.148**.

| Parte | Estado comprobado |
|---|---|
| Request PlayPlay en ESP32 | HTTP 200 documentado previamente; no se repitió hardware en esta sesión |
| Ejecutar VM 148 | Sí: snapshot restaurado o runtime nuevo mediante `0x4a0268` |
| Candidato de 16 bytes en `0x49f854` | Estable, control natural positivo, **0/11** coincidencias con AES conocidas |
| Salida final de VM | **28 bytes**, primeros 24 variables; no es una AES en claro validada |
| Generador de bloques `0xd9d0f0` | Copiar el contexto reproduce exactamente un bloque de reproducción real |
| Repeticiones sin otra canción | 2 entradas naturales + 11 vectores, dos ejecuciones por caso, cuatro bloques estables |
| Servicio `/deob` | Prototipo no validado; prueba real HTTP 500; se dejó detenido |
| Token E incompatible con 148 | Hipótesis pendiente, **no demostrado** por estos resultados |
| E2E cspot/ESP32 con PlayPlay | Pendiente |

La ejecución nativa funciona; **la extracción de AES sigue abierta**. Un control
positivo de repetibilidad no demuestra qué bytes son la clave ni qué contador
usa el generador. No retomar cambiando el token/build o desplegando el servidor
antiguo como si esa parte estuviera resuelta.

## Orden de lectura

1. [PLAN.md](PLAN.md): siguiente experimento y condiciones para avanzar.
2. [task.md](task.md): checklist operativo, subordinado al plan.
3. [Resumen de sesión](docs/WINDOWS_WORK_SUMMARY.md): logros, límites, estado remoto.
4. [Arquitectura 148](docs/PLAYPLAY_ARCHITECTURE_NOTES.md): flujo, ABI, buffers y teorías.
5. [Hechos y hashes](docs/FACTS.md): versión exacta, tokens, RVAs, integración.
6. [Decisiones y descartes](docs/DECISIONS_148.md): qué no repetir y por qué.
7. [Catálogo de scripts](tools/README.md): comandos, dependencias, eventos y códigos de salida.
8. [Evidencia de la sesión](docs/VALIDATION_148_2026-09-24.md): ensayos y enlaces a JSON/logs.

[BACKGROUND.md](docs/BACKGROUND.md) conserva el contexto anterior.
[RVA_DISCOVERY_PLAYBOOK.md](docs/RVA_DISCOVERY_PLAYBOOK.md) explica cómo contrastar
o portar direcciones; no es prueba de que otro build deobfusque correctamente.

## Cómo retomar sin pedir otra canción

Leer el código de `tools/check_key_pipeline_148.js` y ejecutar el comando del
[catálogo](tools/README.md#prueba-preferida-sin-cambiar-de-cancion), con un PID
principal recién verificado y un nombre de informe nuevo. Usa los fixtures ya
guardados; requiere el proceso oficial 148 inicializado. **No se probó aún desde
un arranque frío ni con otro build.** Para un nuevo control natural sí se necesita
reproducción local en **Windows**, no en el cliente Linux ni en otro dispositivo
seleccionado por Connect.

## Restricciones y conservación

- Investigación doméstica de interoperabilidad autorizada. Las credenciales de
  cspot no deben viajar al servicio LAN: solo claves ofuscadas/identificadores.
  La sesión de Spotify ya iniciada en Windows no cambia esa restricción.
- Antes de editar cspot C++, presentar el hallazgo y plan al usuario y esperar
  su autorización. Esta sesión de diagnóstico/documentación no lo modificó.
- SSH exclusivamente con la clave de [acceso documentado](../acceso-ssh-pc-windows.md).
- No publicar un oráculo `/deob` en Internet ni reabrir túneles públicos.
- No sobrescribir resultados anteriores. Los JSON son evidencia primaria;
  algunos scripts evolucionaron después de producirlos. Ver sus límites en el catálogo.
- Conservar los cambios ajenos y el índice Git: parte del árbol ya estaba staged.
- Binarios grandes en `../dlls/`, no en esta carpeta. No asumir que `research/`
  está ignorado por Git; comprobar el estado real antes de un commit/publicación.
