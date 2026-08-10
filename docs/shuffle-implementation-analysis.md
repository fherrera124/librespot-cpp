# Shuffle: análisis de viabilidad y alcance

Análisis de la rama `dev` (motor `cspot`, `TrackQueueHandler` +
`ConnectStateHandler`) sobre implementar shuffle real, usando
`go-librespot` (clonado en el directorio padre del repo,
`../go-librespot`) como referencia. Cubre: qué existía antes, cómo lo
resuelve go-librespot, en qué difiere la arquitectura de cspot (y por qué
esa diferencia simplifica el problema), viabilidad en el target ESP32, y
el diseño con su alcance.

**Estado: Fases 1 y 2 (§8) implementadas y compilando** (`cspot`/
`cspot_cli`, target CLI) — `TrackQueueHandler.cpp` y
`ConnectStateHandler.cpp`. Fase 3 (verificación en hardware real,
incluido el target ESP32) pendiente, a cargo del usuario.

## 1. Estado actual en cspot

Shuffle está enganchado en el protocolo pero es un no-op real:

- `TrackQueueHandler::enableShuffle(bool)` es un stub:
  `return {};  // TODO: Implement shuffle` (`TrackQueueHandler.cpp:755-757`).
- `ConnectStateHandler` ya recibe y despacha `set_shuffling_context`,
  `set_options` y `player_options_override` (dentro de `play`) hacia
  `applyPlayerOptionsLocked()` (`ConnectStateHandler.cpp:300-310,
  305-309, 890-892`), pero esa función es **puramente de estado**: sólo
  pisa `options.shufflingContext` en el `PlayerState` que se reporta a
  Spotify (`ConnectStateHandler.cpp:1262-1275`). Nunca llama a
  `trackQueueHandler->enableShuffle()`. El comentario en el header ya lo
  documenta explícitamente: *"State-only for shuffle: syncs what the
  client displays, doesn't reorder the queue"* (`ConnectStateHandler.h:255-257`).
- `handleTransferCommandLocked()` **no copia** `transferState.options`
  (que sí trae `shufflingContext`, ya decodificado —
  `ConnectPb.h:447-448`) hacia `playerState.options`. Gap ya señalado en
  el propio código: *"options (shuffle/repeat) not copied from
  transferState here ... a known gap, not yet fixed"*
  (`ConnectStateHandler.cpp:714-717`). Relevante para shuffle: si no se
  arregla, un dispositivo que transfiere una sesión con shuffle ya
  activo en el origen pierde ese estado al aterrizar en cspot.

Modelo de datos relevante (`TrackQueueHandler.cpp`):

- `contextPages`: vector de páginas; cada página guarda vectores
  paralelos `trackGids`/`trackUids`/`trackArtistUris`/`trackAlbumUris`.
  Las páginas se resuelven perezosamente — sólo se pide la siguiente
  cuando `ensureEnoughTracks()` detecta que quedan menos de
  `trackFetchThreshold` (8) tracks por delante (`TrackQueueHandler.cpp:15-17,
  340-379`). Un contexto grande (playlist de miles de tracks) normalmente
  **nunca** se materializa por completo salvo que el usuario navegue
  hasta el final.
- `contextIndex = {page, track}`: identidad física **estable** del track
  actual — un par que apunta directo a `contextPages[page]`. No es una
  posición en un array reordenable, es una referencia a dónde vive ese
  track dentro de las páginas ya resueltas.
- `getOffsetIndex(offset)`: navega ese modelo de páginas caminando hacia
  adelante/atrás por `contextPages`, cruzando límites de página según
  haga falta (`TrackQueueHandler.cpp:759-833`). Es el único punto que
  calcula "el track N posiciones desde el actual", usado tanto por
  `skipToNextTrack()`/`skipToPreviousTrack()` como por
  `updateTrackWindows()` para construir las ventanas next/previous.
- La cola manual (`queue`/`isPlayingQueue`, agregada vía `addToQueue`/
  `setQueue`) es un mecanismo aparte, no relacionado con `contextPages`.

## 2. Cómo lo resuelve go-librespot

`tracks/tracks.go` (`List`) envuelve un `pagedList[T]`
(`tracks/paged-list.go`): un array plano (`list []pagedListItem[T]`,
`pos int`) que también pagina perezosamente (`fetchNextPage`), pero a
diferencia de cspot expone operaciones que tratan ese array como una
estructura **físicamente reordenable**.

`ToggleShuffle(ctx, shuffle)` (`tracks.go:291-359`):

- **Activar**: primero drena *todas* las páginas restantes
  (`iter := tl.tracks.iterStart(); for iter.next(ctx) {}` — sin límite,
  hasta `io.EOF`). Genera una semilla aleatoria (`shuffleSeed`) y hace un
  Fisher-Yates completo sobre el array (`pagedList.shuffle()`,
  `paged-list.go:135-151`), llevando la cuenta de a dónde termina moviéndose
  la posición actual (`pos`). Si `pos` era una posición real (>0), la
  intercambia a la posición 0 (`swap(0, pos)`) — así el track que estaba
  sonando sigue sonando, y el resto queda mezclado alrededor. Guarda
  `shuffleLen` (cantidad de tracks al momento de mezclar).
- **Desactivar**: si la semilla sigue siendo válida y la cantidad de
  tracks no cambió (`tl.tracks.len() == tl.shuffleLen` — nadie modificó
  la playlist mientras estaba mezclada), revierte el swap inicial y
  aplica `unshuffle()` (`paged-list.go:153-174`): reproduce la *misma*
  secuencia de intercambios que generó `shuffle()` con el mismo seed
  (reinicializando el RNG), pero los aplica en **orden inverso** — el
  truco estándar para invertir un Fisher-Yates. Si la playlist cambió
  (o no hay semilla), no puede confiar en revertir: recuerda el track
  actual, limpia la lista, re-hace fetch de las páginas en orden original
  y busca (`Seek`) ese track de nuevo.
- `Index()` (`tracks.go:169-176`) devuelve `pageIdx`/`itemIdx`
  **originales** — cada `pagedListItem[T]` guarda esa metadata pegada al
  item cuando se creó, y el swap sólo mueve la *posición* del item en el
  slice, no su metadata. Es decir: el campo `index` que se reporta a
  Spotify es siempre la posición física-original del track dentro del
  contexto, nunca su posición dentro del orden mezclado.

Integración (`daemon/controls.go`, `daemon/player.go`):

- `loadContext()` (control local, `controls.go:253-328`): si **no** hay
  track objetivo explícito (`skipTo == nil` — "play esta playlist" sin
  más), mezcla primero y *después* hace `Seek` a "el primero que
  encuentre" — como la lista ya está mezclada, eso es efectivamente un
  track aleatorio. Si **sí** hay un objetivo explícito, busca ese track
  primero y mezcla después (preserva el track pedido en la posición 0).
  También respeta `Context.Restrictions.DisallowTogglingShuffleReasons`
  forzando `ShufflingContext = false` si el contexto no permite shuffle.
- Transferencia (`player.go:220-305`): copia `Options` completo desde
  `TransferState` (`p.state.player.Options = transferState.Options`,
  línea 235), busca el track transferido (`TrySeek`), y **después**
  llama `ToggleShuffle` con el valor ya copiado — mismo orden
  seek-then-shuffle que el caso "con objetivo explícito" de arriba.
- Toggle en vivo (`setOptions()`, `controls.go:503-562`): sólo llama
  `ToggleShuffle` si el valor realmente cambió, reconstruye
  Track/PrevTracks/NextTracks/Index, invalida cualquier prefetch del
  siguiente track (`secondaryStream = nil`) y dispara `updateState()`.

## 3. Diferencia arquitectónica clave — por qué cspot no necesita el truco reversible

go-librespot necesita semilla + reproducir-e-invertir los intercambios
porque su array (`pagedList.list`) **es** la fuente de verdad del orden:
una vez mezclado in-place, la única forma de volver al orden original es
deshacer matemáticamente esos intercambios (o re-fetch todo desde cero).

cspot ya identifica cada track por una referencia física estable,
`{page, track}`, independiente de en qué orden se lo recorra. Eso abre
una implementación más simple: en vez de reordenar `contextPages` (mover
strings/gids de posición), basta con una **capa de indirección** — una
secuencia de índices lógicos que apunta a esos pares físicos, usada sólo
mientras el shuffle está activo. Apagar shuffle es entonces trivial: se
deja de consultar esa secuencia y `contextIndex` ya es, por construcción,
la posición física real — no hace falta revertir nada ni volver a pedir
páginas. Ver diseño en la §5.

## 4. Viabilidad en el target ESP32 (`targets/esp32`, ESP32-S3 + PSRAM octal)

- **Memoria**: `PageMetadata.trackCount` (`ContextPageParser.h:13`) es
  el conteo de *esa página*, no hay un total del contexto disponible por
  adelantado — el único modo de saber cuánto pesa un contexto es
  terminar de pedirlo. Por track guardado hoy: 16 bytes de gid + 3
  strings (`uid`, `artistUri`, `albumUri`, típicamente cortos) — del
  orden de 100-150 bytes reales con overhead de `std::string`/`vector`.
  Una capa de indirección de shuffle sólo necesita un par de índices por
  track (8 bytes si son `uint32_t`). Para un contexto de varios miles de
  tracks (una playlist grande, o "Liked Songs"), el total se queda en el
  orden de un dígito bajo de MB — factible con la PSRAM octal ya
  habilitada (`targets/esp32/sdkconfig.defaults:14-18`,
  `CONFIG_SPIRAM_MODE_OCT`), pero **sin cota conocida de antemano**, a
  diferencia de go-librespot corriendo en desktop/RPi. Recomendación:
  agregar un cap defensivo (p. ej. algunos miles de tracks) al loop de
  "traer todo el contexto" — algo que go-librespot no necesita y cspot
  sí, dado el target.
- **Red — revisado con evidencia real**: la preocupación inicial era
  "docenas de round-trips para activar shuffle". Los fixtures reales de
  `test/data/context-resolve/` (capturas reales, usadas por
  `test/TrackQueueHandlerTest.cpp`) muestran que una playlist normal
  (`daylist.json`, 50 tracks) vuelve completa en **una sola página, sin
  paginar** — el propio test lo dice: `// No pagination in a playlist
  context`. Es decir, para el caso común (playlist/álbum), "traer todo
  el contexto" no cuesta requests *adicionales*: cspot ya recibe el
  100% en el fetch de la página raíz que hace hoy en cada
  `loadContext()`, con o sin shuffle. Los únicos fixtures multi-página
  son `artist` (cada "página" es un álbum distinto del artista, no un
  chunk de una lista plana) y `station`/`daylist-autoplay` (radio
  algorítmica, **infinita** — no tiene "todo el contexto" que traer, y
  shuffle no debería ni ofrecerse ahí). Queda como incógnita real sin
  evidencia local: si una playlist de cientos/miles de tracks empieza a
  paginar en algún punto — no hay fixture de ese tamaño. El cap
  defensivo de abajo cubre ese caso sin necesidad de resolverlo de
  antemano.
- **Lock**: ese fetch, igual que el de `loadContext()` hoy
  (`ConnectStateHandler.cpp:758-765`), correría con `putStateMutex` ya
  tomado por el caller — mismo patrón ya aceptado en el resto del
  archivo, no es un riesgo nuevo, pero sí implica que toggle de shuffle
  bloquea el resto de comandos/`onPlayerStateUpdate()` por la duración
  del fetch. Razonable reusar `isBuffering=true` mientras dura, como ya
  hace `transfer`.

## 5. Diseño — `TrackQueueHandler`

Estado nuevo, privado a `DefaultTrackQueueHandler` (junto a `contextIndex`):

```cpp
bool shuffled = false;
std::vector<cspot_proto::ContextIndex> shuffleOrder;  // permutación de pares (page,track) físicos
size_t shufflePos = 0;
```

Se reutiliza `cspot_proto::ContextIndex` (ya usado como `{page, track}`
en todo el archivo, p. ej. `cspot_proto::ContextIndex{0, 0}`) en vez de
introducir un `std::pair` nuevo — un tipo menos, cero conversión en los
sitios que ya construyen/comparan este par.

### 5.1 Método libre: Fisher-Yates (namespace anónimo, junto a `uriToGid`)

Igual que `uriToGid`, una función pura y testeable sin `TrackQueueHandler`
completo — sólo recibe el array y el índice a preservar:

```cpp
// Baraja `order` in-place, siguiendo a `pinIndex` (un índice DENTRO de
// `order`, no un {page,track}) a través de los swaps para que el
// llamador pueda ubicar dónde terminó.
void fisherYatesShuffle(std::vector<cspot_proto::ContextIndex>& order,
                        size_t& pinIndex,
                        std::default_random_engine& rng) {
  if (order.size() <= 1) {
    return;
  }
  for (size_t i = order.size() - 1; i > 0; i--) {
    size_t j = std::uniform_int_distribution<size_t>(0, i)(rng);
    std::swap(order[i], order[j]);
    if (i == pinIndex) {
      pinIndex = j;
    } else if (j == pinIndex) {
      pinIndex = i;
    }
  }
}
```

El motor aleatorio sigue el idiom ya usado en este mismo subárbol
(`StreamPlayer.cpp:9-12`, `generatePlaybackId()`): `static
std::default_random_engine` sembrado una vez con `std::random_device`,
en vez de traer una dependencia/patrón nuevo.

### 5.2 Fetch completo, acotado

```cpp
// tracks/tracks - sin contador de tracks total expuesto por la API
// (§4) - así que el único modo de detectar un contexto demasiado
// grande es notarlo mientras se pagina. go-librespot no tiene
// equivalente (memoria de desktop/RPi sin restricción); el target de
// este repo incluye ESP32.
const uint32_t maxShuffleTracks = 20000;

bell::Result<> DefaultTrackQueueHandler::fetchAllContextPages() {
  size_t totalTracks = 0;
  for (auto& page : contextPages) {
    totalTracks += page.trackGids.size();
  }

  // Índice, no range-for: contextPages puede crecer dentro del propio
  // fetchContextPage() (onPageMetadataParsed() hace push_back al
  // descubrir next_page_url) - un range-for cachea begin()/end() antes
  // del loop y quedaría con iteradores colgantes si el push_back
  // realloca. Mismo idiom defensivo que ya usa ensureEnoughTracks().
  size_t pageIndex = 0;
  while (pageIndex < contextPages.size()) {
    if (contextPages[pageIndex].trackGids.empty()) {
      auto res = fetchContextPage(contextPages[pageIndex]);
      if (!res) {
        return nonstd::make_unexpected(res.error());
      }
      totalTracks += contextPages[pageIndex].trackGids.size();
      if (totalTracks > maxShuffleTracks) {
        BELL_LOG(error, LOG_TAG,
                 "Context too large to shuffle (>{} tracks), aborting",
                 maxShuffleTracks);
        return bell::make_unexpected_errc(std::errc::value_too_large);
      }
    }
    pageIndex++;
  }
  return {};
}
```

> **Nota al margen, no bloqueante**: `loadContext()` (líneas 244-265)
> recorre `contextPages` con un range-for que también puede disparar
> `push_back` dentro del cuerpo (vía `fetchContextPage()` →
> `onPageMetadataParsed()`) — el mismo riesgo de iteradores colgantes
> que el comentario de arriba describe, ya presente hoy, sin relación
> con shuffle. No se toca como parte de este trabajo; queda anotado acá
> por aparecer en el mismo archivo/mecanismo.

### 5.3 `enableShuffle()` real

```cpp
bell::Result<> DefaultTrackQueueHandler::enableShuffle(bool shuffle) {
  if (shuffle == shuffled) {
    return {};
  }

  if (!shuffle) {
    shuffled = false;
    shuffleOrder.clear();
    shufflePos = 0;
    return {};  // contextIndex ya es la posición física real
  }

  if (!contextIndex) {
    return {};  // nada cargado (sesión sólo-cola / ad-hoc)
  }

  auto res = fetchAllContextPages();
  if (!res) {
    return res;
  }

  std::vector<cspot_proto::ContextIndex> order;
  size_t pinIndex = 0;
  bool foundPin = false;
  for (uint32_t page = 0; page < contextPages.size(); page++) {
    for (uint32_t track = 0; track < contextPages[page].trackGids.size(); track++) {
      if (page == contextIndex->page && track == contextIndex->track) {
        pinIndex = order.size();
        foundPin = true;
      }
      order.push_back({page, track});
    }
  }

  if (!foundPin) {
    // No debería pasar - contextIndex siempre apunta a un track ya
    // fetcheado - pero degradar a "shuffle no tuvo efecto" es más
    // seguro que dejar shufflePos apuntando a algo que no matchea.
    BELL_LOG(error, LOG_TAG, "Could not locate current track while shuffling");
    return {};
  }

  static std::default_random_engine rng{std::random_device{}()};
  fisherYatesShuffle(order, pinIndex, rng);

  if (pinIndex != 0) {
    std::swap(order[0], order[pinIndex]);  // preserva el track actual en 0
  }

  shuffleOrder = std::move(order);
  shufflePos = 0;
  shuffled = true;
  return {};
}
```

### 5.4 Centralizar el movimiento del cursor (evita esparcir `if (shuffled)`)

Todo sitio que hoy mueve `contextIndex` un offset relativo, o lo resetea
al inicio del contexto, pasa a usar dos helpers chicos — mismo criterio
de extracción que ya usó este archivo para
`announcePlaybackFlagsLocked()`/`refreshTrackAndIndexLocked()` (commits
`3db33eee`/`e3d86aaf`), acá aplicado a que `contextIndex` y `shufflePos`
nunca puedan desincronizarse por olvidarse de actualizar uno de los dos
en algún call site:

```cpp
// Único lugar que mueve el cursor un offset relativo - mantiene
// contextIndex y (si está mezclado) shufflePos sincronizados siempre
// juntos, nunca por separado.
bool DefaultTrackQueueHandler::advanceContextBy(int32_t offset) {
  auto next = getOffsetIndex(offset);
  if (!next) {
    return false;
  }
  contextIndex = *next;
  if (shuffled) {
    shufflePos = static_cast<size_t>(static_cast<int64_t>(shufflePos) + offset);
  }
  return true;
}

// "Volver al inicio del contexto" - shuffleOrder[0] si está mezclado
// (no el {0,0} físico, o cada vuelta de repeat-context delataría el
// shuffle volviendo siempre al mismo track físico), {0,0} si no.
void DefaultTrackQueueHandler::resetContextToStart() {
  if (shuffled && !shuffleOrder.empty()) {
    shufflePos = 0;
    contextIndex = shuffleOrder[0];
  } else {
    contextIndex = cspot_proto::ContextIndex{0, 0};
  }
}

// Límite "inicio de contexto" para skipToPreviousTrack().
bool DefaultTrackQueueHandler::atContextStart() const {
  if (shuffled) {
    return shufflePos == 0;
  }
  return contextIndex->page == 0 && contextIndex->track == 0;
}
```

`getOffsetIndex(offset)` gana una única rama nueva al principio (antes
de tocar `contextPages`):

```cpp
if (shuffled) {
  int64_t newPos = static_cast<int64_t>(shufflePos) + offset;
  if (newPos < 0 || newPos >= static_cast<int64_t>(shuffleOrder.size())) {
    return std::nullopt;
  }
  return shuffleOrder[static_cast<size_t>(newPos)];
}
// ... resto de la función, sin cambios (recorrido físico por páginas)
```

Con eso, los call sites existentes se simplifican en vez de crecer:

- `skipToNextTrack()` (rama de contexto): `getOffsetIndex(1)` +
  `contextIndex = *nextIndex` → `advanceContextBy(1)`; el wrap
  (`contextIndex = {0,0}`) → `resetContextToStart()`.
- `skipToTargetTrack()`: el offset ya calculado
  (`x - queueOffset + 1`) pasa por `advanceContextBy(...)` en vez de
  `getOffsetIndex(...)` + asignación manual; el fallback "no encontrado"
  → `resetContextToStart()`.
- `skipToPreviousTrack()`: el chequeo `page==0 && track==0` →
  `atContextStart()`; `getOffsetIndex(-1)` + asignación → `advanceContextBy(-1)`.
- `updateTrackWindows()`: **sin cambios** — ya construye next/previous
  llamando `getOffsetIndex()`, se vuelve shuffle-aware gratis.
- `currentContextIndex()`: **sin cambios** — sigue reportando la
  posición física real (`contextIndex`), igual que `Index()` en
  go-librespot (§2) reporta siempre `pageIdx`/`itemIdx` originales.

### 5.5 Reset y resincronización

- `resetContext()` (usado por `clearContext()` y por el branch de
  contexto-nuevo de `loadContext()`) gana tres líneas:
  `shuffled = false; shuffleOrder.clear(); shufflePos = 0;` — un
  contexto distinto invalida cualquier permutación anterior.
- **Caso borde real, no hipotético**: `loadContext()` con la MISMA
  `contextUri` pero un target distinto (usuario clickea otro track
  dentro de la playlist ya abierta, o una transferencia vuelve a
  aterrizar en el mismo contexto) — hoy esa rama (líneas 211-242)
  resuelve un `contextIndex` físico nuevo sin tocar `resetContext()`
  (a propósito: reusa la caché de `contextPages`). Si está mezclado, el
  `shuffleOrder` sigue siendo válido (mismo set de tracks) pero
  `shufflePos` queda apuntando al track VIEJO. Un helper chico lo
  arregla, único call site al final de esa rama:

```cpp
// Reubica shufflePos para que matchee el contextIndex físico recién
// resuelto (loadContext(), rama "mismo contexto, target distinto").
// Si el track ya no está en shuffleOrder (la playlist se editó entre
// medio), el shuffle queda stale - se apaga en vez de dejar shufflePos
// apuntando a cualquier cosa. Mismo espíritu que la precondición de
// go-librespot para su unshuffle reversible (tracks.go:325, "len sin
// cambios") - acá se aplica al re-sync en vez de al apagado.
void DefaultTrackQueueHandler::syncShufflePosToContextIndex() {
  if (!shuffled || !contextIndex) {
    return;
  }
  auto it = std::find_if(shuffleOrder.begin(), shuffleOrder.end(),
                         [&](const cspot_proto::ContextIndex& idx) {
                           return idx.page == contextIndex->page &&
                                  idx.track == contextIndex->track;
                         });
  if (it != shuffleOrder.end()) {
    shufflePos = std::distance(shuffleOrder.begin(), it);
  } else {
    shuffled = false;
    shuffleOrder.clear();
  }
}
```

- La cola manual (`queue`/`isPlayingQueue`) no se toca — shuffle sólo
  reordena la secuencia del contexto, igual que en go-librespot
  (`ToggleShuffle` nunca toca `tl.queue`).
- `resolveFlatIndex()` (sólo usado para `skip_to.track_index` explícito
  al cargar contexto) sigue resolviendo contra el layout físico, sin
  cambios — ver alcance en §7.

### 5.6 SOLID, explícito

- **SRP**: la lógica de secuenciación de shuffle vive en 4 métodos
  privados chicos y de propósito único (`fetchAllContextPages`,
  `advanceContextBy`, `resetContextToStart`, `atContextStart`,
  `syncShufflePosToContextIndex`) en vez de esparcir `if (shuffled)` en
  cada sitio que toca `contextIndex` — mismo patrón de extracción que ya
  usa este proyecto. El Fisher-Yates en sí es una función libre,
  testeable sin un `TrackQueueHandler` completo.
- **OCP**: se consideró generalizar `getOffsetIndex()` con un patrón
  Strategy para el "modo de secuenciación" y se descarta — con sólo dos
  modos y una rama `if (shuffled)`, una jerarquía de estrategias es
  complejidad sin beneficio real. Mismo criterio que "Opción 3 — Tabla
  de despacho — won't fix" en `put-state-architecture-analysis.md`.
- **LSP/ISP**: `enableShuffle(bool)` ya existe en la interfaz
  `TrackQueueHandler` con esta firma exacta — cero cambios de contrato,
  interfaz ya mínima.
- **DIP**: `ConnectStateHandler` ya depende de la interfaz
  `TrackQueueHandler` (inyectada desde `9c28d5a2`) — invocar
  `enableShuffle()` real no agrega acoplamiento nuevo.

## 6. Integración en `ConnectStateHandler`

`applyPlayerOptionsLocked()` es el único punto de entrada para los 5
call sites que tocan shuffle (`set_repeating_track`,
`set_shuffling_context`, `set_options`, `player_options_override`
dentro de `play`, y el nuevo en `transfer` de abajo) — ponerle el
wiring ahí, una sola vez, alcanza para los cinco:

```cpp
void ConnectStateHandler::applyPlayerOptionsLocked(
    std::optional<bool> repeatingContext, std::optional<bool> repeatingTrack,
    std::optional<bool> shufflingContext) {
  auto& options = putStateRequestProto.device.playerState.options;
  if (repeatingContext) {
    options.repeatingContext = *repeatingContext;
  }
  if (repeatingTrack) {
    options.repeatingTrack = *repeatingTrack;
  }
  if (shufflingContext && *shufflingContext != options.shufflingContext) {
    auto res = trackQueueHandler->enableShuffle(*shufflingContext);
    if (!res) {
      BELL_LOG(error, LOG_TAG, "Could not toggle shuffle to {}: {}",
               *shufflingContext, res.error());
      // options.shufflingContext queda en su valor anterior a propósito
      // - el toggle no tomó efecto, no correspondería reportarlo como
      // si hubiera tomado. Mismo criterio que setOptions() en
      // go-librespot (retorna temprano en error, sin tocar
      // Options.ShufflingContext) - pero acá NO se propaga como fallo
      // duro del comando completo (ver nota de abajo).
    } else {
      options.shufflingContext = *shufflingContext;
      trackQueueHandler->updateTrackWindows(/*forceNotify=*/true);
      refreshTrackAndIndexLocked();
    }
  }
}
```

Se mantiene `void` (no `bell::Result<>`) — el fallo se maneja adentro,
no se propaga; ver la nota siguiente sobre por qué.

**Decisión deliberada, distinta de go-librespot**: en el path de
transferencia, go-librespot trata un `ToggleShuffle` fallido como error
duro (`return fmt.Errorf("failed shuffling context")`, aborta toda la
transferencia). Acá no — dado el resto del archivo ya prioriza "degradar
en vez de romper" (múltiples comentarios existentes en ese sentido), un
fetch de shuffle fallido (red, o el cap de tamaño) no debería impedir
que la transferencia/reproducción arranque; simplemente queda sin
mezclar y loggeado. Un usuario prefiere reproducción sin shuffle a que
la transferencia entera falle por eso.

**Fix acompañante — gap de `transferState.options`** (§1): en
`handleTransferCommandLocked()`, justo antes de las líneas compartidas
`updateTrackWindows(); refreshTrackAndIndexLocked();` (hoy 807-808),
agregar:

```cpp
applyPlayerOptionsLocked(transferState.options.repeatingContext,
                         transferState.options.repeatingTrack,
                         transferState.options.shufflingContext);
```

Reusa la misma función (cero código nuevo en este punto), y corre
**después** de que `loadContext()`/`setQueue()`/`setPlayingQueue()` ya
resolvieron el track transferido (líneas 748-805) — el mismo orden
"seek primero, shuffle después" que go-librespot usa en su path de
transferencia (§2), así que el track transferido queda preservado en la
posición 0 del shuffle. Los tres campos de `TransferState.options` son
`bool` planos, no `optional` — pasarlos siempre "presentes" es correcto
acá: una transferencia establece el estado del reproductor desde cero,
no hace merge incremental sobre uno ya activo (a diferencia de
`set_options`, donde `nullopt` sí significa "no tocar").

**Caso fuera de alcance para v1, documentado en vez de resuelto**: "play
esta playlist con shuffle ya activo, sin track explícito" (go-librespot
mezcla *antes* de elegir el primer track, así que arranca en uno
aleatorio). En este flujo, `loadContext()` ya deja `contextIndex` en el
default físico `{0,0}` antes de que `applyPlayerOptionsLocked()`
corra — `enableShuffle(true)` va a preservar ESE track en la posición 0,
no uno al azar. No es incorrecto (el resto de la playlist sí queda
mezclado), sólo menos "aleatorio" que Spotify real en ese caso puntual.
Arreglarlo requeriría que `enableShuffle()` supiera distinguir "hay un
target real que preservar" de "no lo hay" — la firma actual no lo
necesita para nada de lo que sí está en alcance (§7), así que se deja
afuera en vez de anticipar una API más compleja sin un caso de uso
confirmado.

## 7. Alcance de este plan

**Incluido:**

- Toggle en vivo (`set_shuffling_context`, `set_options`,
  `player_options_override` con track objetivo explícito) — preserva el
  track actual, reordena sólo lo que viene después.
- Transferencia que llega con shuffle ya activo en el origen (incluye el
  fix del gap `transferState.options`).
- Wrap-to-start (fin de contexto + repeat, target no encontrado) y
  re-target dentro del mismo contexto (§5.5), ambos shuffle-aware.
- Cap defensivo (`maxShuffleTracks`) para el fetch completo.

**Fuera de alcance, documentado como gap conocido (mismo criterio ya
usado en el repo para `Restrictions`/`ContextRestrictions` — ver
`docs/connect-state-investigation.md`, sección `toggle-repeat/shuffle
reason`):**

- Restricciones de contexto (`DisallowTogglingShuffleReasons`) — no hay
  binding de `Restrictions` en `ConnectPb.h` hoy; `master` tampoco lo
  implementa y nunca surgió como causa de bug en hardware real. Efecto
  práctico: cspot permitiría togglear shuffle en un contexto que
  Spotify marcaría como no-shuffleable (radio/autoplay) — mitigado
  parcialmente porque esos contextos son infinitos y `enableShuffle()`
  ya fallaría/abortaría al no poder agotar el fetch (§5.2), no por
  chequear la restricción explícitamente.
- `skip_to.track_index` numérico bajo contexto ya mezclado —
  go-librespot tampoco le da tratamiento especial.
- "Play esta playlist" con shuffle ya activo y sin track explícito
  arrancando en posición aleatoria (§6, último punto).

## 8. Plan de implementación, en fases

**Fase 1 — DONE — `TrackQueueHandler` (autocontenida, no toca `ConnectStateHandler`)**

1. Estado nuevo (§5) + `fisherYatesShuffle()` libre + `fetchAllContextPages()`.
2. `enableShuffle()` real (§5.3).
3. `getOffsetIndex()` gana la rama shuffle (§5.4).
4. `advanceContextBy()`/`resetContextToStart()`/`atContextStart()` +
   reemplazar los usos actuales en `skipToNextTrack()`/
   `skipToPreviousTrack()`/`skipToTargetTrack()`.
5. `resetContext()` limpia el estado de shuffle; `syncShufflePosToContextIndex()`
   + su call site en `loadContext()` (§5.5).
6. Verificable de forma aislada con el target CLI: cargar un contexto,
   togglear, comparar `nextTracks()`/`previousTracks()` antes/después en
   los logs.

**Fase 2 — DONE — Wiring en `ConnectStateHandler`**

1. `applyPlayerOptionsLocked()` invoca `enableShuffle()` real (§6).
2. Fix del gap `transferState.options` en `handleTransferCommandLocked()`.
3. Nada más cambia — los call sites existentes siguen iguales, sólo que
   ahora `shufflingContext` tiene efecto real.

**Fase 3 — pendiente — Verificación en hardware real** (build/flash/monitor
los hace el usuario, según el flujo ya establecido):

1. Toggle en vivo de shuffle a mitad de playlist — el track actual no
   debe cambiar, `nextTracks()` sí.
2. Transferencia desde el celular con shuffle ya prendido en origen.
3. Repeat-context + shuffle activo — confirmar que la vuelta cae en
   `shuffleOrder[0]`, no en el track físico #1.
4. Mezclar, y clickear otro track de la MISMA playlist (mismo contexto)
   — confirmar que `shufflePos` quedó sincronizado (caso §5.5).

No se incluyen tests en `test/TrackQueueHandlerTest.cpp` como parte de
este plan — ese target no compila hoy (falta `doctest`, gap ya conocido,
no se persigue el fix acá). El Fisher-Yates libre (§5.1) queda escrito
de forma que sea trivial testear en cuanto ese target funcione.
