# Checklist operativo — PlayPlay LAN

Corte **2026-09-24**. [PLAN.md](PLAN.md) es la fuente de verdad.
`[x]` indica el resultado limitado que describe cada renglón, no éxito E2E.

## Request y ejecución nativa

- [x] Request token E/version 5 con HTTP 200, documentado en hardware previamente.
- [x] Registrar build/hash, PID principal y servicio remoto previo.
- [x] Ejecutar VM 148 con snapshot de 144 bytes y excepciones propagadas.
- [x] Corregir trazado de llamadas repetidas: `setImmediate`, `traps: all`, ámbito por hilo.
- [x] Localizar copia `0x17780e0` y candidato RDX en `0x49f854`.
- [x] Control natural del candidato y 11 × 2 repeticiones: estable, 0/11 coincidencias.
- [x] Trazar fuentes de copias >=8 bytes: 13 lotes, sin AES conocidas encontradas.
- [x] Capturar licencia/recurso/callback en precarga y reproducción de Windows.
- [x] Seguir estáticamente callback de reproducción hasta `0xd9e2e4` / `0xd9d0f0`.
- [x] Reconstruir runtime por `0x4a0268`: 13 casos × 2, cuatro bloques estables.
- [x] Repetir exactamente un bloque natural con snapshot de contexto `0x2e4`.

## Extracción AES — pendiente principal

- [/] Relacionar estado codificado, contador y auxiliar con la AES de contenido.
- [ ] Obtener un control independiente de AES/descifrado ligado a un recurso exacto.
- [ ] Validar vectores por versión; especialmente los dos v5 usados por cspot.
- [ ] Decidir compatibilidad E/148 con ese control; no está demostrada actualmente.
- [ ] Solo si hace falta, capturar token/version de request o preparar otro build.

## Servicio LAN y E2E

- [x] Agregar validador HTTP y comprobarlo con cuatro pruebas locales simuladas.
- [x] Probar el servidor 148: HTTP 500 conservado; servidor de prueba detenido.
- [ ] Sustituir extracción incorrecta/no validada y corregir ejecución RPC.
- [ ] Completar dos pasadas HTTP de 11 vectores y guardar 22 resultados válidos.
- [ ] Verificar vida de memoria, concurrencia y recuperación tras reinicio.
- [ ] Probar licencia fresca/chunk CDN/Vorbis con la AES validada.
- [ ] Presentar plan al usuario, modificar integración C++ y probar hardware.

## Limpieza de integración C++ — sin modificar en esta sesión

- [ ] Retirar `FORCE PLAYPLAY`.
- [ ] Eliminar escritura de credenciales a `/tmp/creds.json`.
- [ ] Sustituir parser manual del RPC y validar hexadecimal/longitud.
- [ ] Retirar hilo de prueba detached con track fijo.

## Handoff

- [x] Consolidar estado, contratos observados, descartes e hipótesis.
- [x] Documentar todos los scripts ejecutados en esta sesión y sus comandos.
- [x] Conservar informes originales y distinguir sus limitaciones de formato.
- [x] Último cierre experimental: hooks desadjuntados, Spotify responde, sin validadores activos.
- [ ] Confirmar PID/build/procesos de nuevo al retomar; esos datos no son permanentes.
