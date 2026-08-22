# Connect-state activation: investigation notes

Working notes from debugging why the ESP32 receiver doesn't reliably take
over playback via Spotify Connect (`transfer` command → PUT
`is_active=true` → the source device is supposed to hand off control).
Kept here so a future session doesn't have to re-derive all of this from
scratch. Started as two issues (one fixed, one open); grew into a
broader audit of the activation path as more candidate causes were
found and ruled in/out. Sections 2-7 chase "ESP32 receives a transfer,
then the phone reclaims it ~25-40s later" - that specific symptom was
never directly reproduced again after the fixes in sections 4-7, so it
was never re-confirmed broken *or* fixed on hardware; treat it as
still-open if it resurfaces. Section 8 is a different bug in the same
code area (a guard on `handleClusterUpdate`, added and removed outside
this doc's own narrative). **Section 9 (later session) found and
confirmed on hardware the root cause of this doc's other running
symptom - "Spotify can't play this file right now" on the PC client
when the ESP32 cedes control back to it** - a different direction of
handoff than sections 2-7, but the same code area and plausibly related
(both are the PC/phone failing to make sense of state the ESP32 sent
it). If section 2's specific reclaim symptom is retested and still
reproduces after section 9's fix, they're confirmed separate causes;
if it doesn't, they likely shared a root cause all along.

## 1. Fixed: stale pooled HTTP connection silently drops requests

### Symptom

```
E SocketStream.cpp:64: Write failed: I/O error
E Client.cpp:219: Error during request write: I/O error
E SpClient.cpp:112: Error while sending request: I/O error
E ConnectStateHandler.cpp:523: Failed to put state
```

Seen ~24 minutes into a session, right as the device tried to PUT
`is_active=true` in response to a `transfer` command. The write failed
outright - the PUT never reached spclient - and nothing above this layer
retried, so `ConnectStateHandler::handleTransferCommand` just logged the
error and moved on as if the activation had been sent.

### Root cause

`bell::http::ConnectionPool` (`external/bell/include/bell/http/Client.h`)
recycles keep-alive sockets per `(host, port)` for up to
`connectionIdleTimeoutSec` (5 minutes, `Client.h:147`). Spotify's spclient
endpoint closes idle keep-alive connections well before that. When
`DefaultTransport::execute()` popped a connection that the *peer* had
already closed, the first `write()` to it failed at the TCP level
(broken pipe), and the code had no notion of "this might just be a dead
pooled socket, try again fresh" - it treated the failure as if it were a
real, permanent network error.

### Fix applied (`external/bell/main/http/Client.cpp`)

- `DefaultTransport::execute()` now retries once on a brand-new
  (non-pooled) connection if the write phase fails on a connection that
  came from the pool. Safe because a write failure at that point means
  the peer never received the request - nothing to duplicate.
- The dead socket is explicitly `close()`d before it's dropped, and
  `ConnectionPool::PoolDeleter` now checks `isValid()` before reinserting
  a returned socket into the pool, so a socket we know is dead can't
  circulate back out to the next caller looking healthy.

This is a generic HTTP-client fix - it covers every caller (SpClient,
CredentialsResolver, ...), not just the Connect-state PUT.

### Why this is a band-aid, and what a more consistent fix would look like

- We still only find out a connection is dead *after* paying for a failed
  write + a full extra connect round-trip. A cleaner approach would
  proactively check reusability before writing - e.g. a non-blocking
  `MSG_PEEK`/`poll()` read-for-0-bytes check (EOF = peer closed) right
  after popping from the pool, or a `select()`-based "is this fd still
  writable/not hung up" probe.
- `connectionIdleTimeoutSec` (5 min) is our own guess and has no
  relationship to spclient's actual keep-alive timeout. Worth either
  discovering the real value empirically (how long can a pooled
  connection sit idle before the *first* stale-write retry fires in
  practice?) and setting our own timeout conservatively below it, or
  dropping the fixed timeout entirely in favor of the proactive check
  above.
- The retry only covers the *write* phase. A connection that dies
  between writing the request and reading back the response (stale mid
  read, e.g. `reader.readHeaders()` failing) is not retried - same class
  of bug, different phase. Worth auditing whether that's been observed in
  practice before investing in it.
- No visibility into how often this actually triggers - a counter/log
  line specifically for "pooled write failed, retried fresh, succeeded"
  vs "...retried fresh, failed again" would help judge whether the pool
  is worth the complexity for spclient's actual traffic volume (PUT
  Connect-state is not high frequency).

## 2. Likely root cause found: `message_id` was never set (fix applied, pending hardware verification)

### Symptom

The device successfully PUTs `is_active=true` (200 OK, "Put state
succeeded"), and in the best case genuinely starts streaming and decoding
real audio from the CDN (Ogg opened, ranges downloading). But somewhere
between ~23 and ~38 seconds after the `transfer` command, an async
`ClusterUpdate` arrives via the dealer saying `active_device_id` is back
to the *source* device (the phone that initiated the transfer), and our
own `stopBeingActive` logic (faithfully mirrors go-librespot's
`daemon/player.go`) correctly self-deactivates in response. From the
user's side: "reproduce en el ESP32 pero el cliente no se entera y sigue
reproduciendo, como si el ESP32 no estuviera conectado."

Reproduced 3 times across separate boots, with the *same* Spotify
account/phone:

| Run | Source state at transfer | Time from transfer to reclaim |
|---|---|---|
| 1 | paused | ~32s |
| 2 | paused (retransmitted transfer at +14s) | ~32s (from 2nd transfer) |
| 3 | **actively playing**, real audio streamed on ESP32 | ~23-38s |

Run 3 rules out "paused handoff times out" as the explanation - it
happens with a genuinely active, audibly-progressing transfer too.

### New data point from run 3

The **very first** PUT response (the one for the `transfer`-triggered
`is_active=true` PUT itself, not a later one) already echoes
`activeDeviceId=<phone>` in its own response body, immediately - not just
30s later via the async push:

```
PUT DIAG: ... isActive=true ...
PUT response cluster: activeDeviceId=27d006c0b352347e4f8fdbe63ee8cc196d134851 (ours=142137fd...)
Put state succeeded in 386ms (reason=4, isActive=true, ...)
```

Caveat: go-librespot never reads this field from the PUT response at all
(`spclient/spclient.go`'s `PutConnectState` closes the body unread on
200), so we don't have a reference implementation's behavior to compare
against here, and it's possible this echo is simply non-authoritative /
eventually-consistent on Spotify's side (i.e. not a real signal either
way). Flagged here because it's a new observation, not because it's
confirmed to mean anything.

### What's been ruled out so far (verified against go-librespot, not guessed)

- `PutStateRequest` field completeness: `is_active`, `put_state_reason`,
  `last_command_message_id`/`last_command_sent_by_device_id`,
  `NEW_DEVICE` PUT on every `hm://pusher/v1/connections` (including
  reconnects) - all present, field numbers match
  `go-librespot/proto/spotify/connectstate/connect.proto` exactly.
- `Cluster.active_device_id` is wire field 2 in both protos - our
  parsing of it is trustworthy, not a decode bug.
- `stopBeingActive` (`ConnectStateHandler.cpp:356-359`) is a term-for-term
  match of go's `daemon/player.go:163` formula.
- `is_active` (`bool`, wire field 4) is a `pb_callback_t` in the
  generated nanopb struct (message-level `FT_CALLBACK`), and our
  `pbEncodeVarint<bool>` callback is unconditionally bound and always
  writes the tag+value - no obvious reason for it to silently not reach
  the wire.
- `capabilities.supports_gzip_pushes = false` differs from go's `true`,
  but `DealerClient` has no gzip decompression at all, so `false` is
  actually correct for us - flipping it would break parsing of any
  compressed push, not fix anything.
- The HTTP pool bug above (section 1) - fixed, but did *not* resolve this
  issue (run 3, above, is post-fix).

### Leading hypotheses, unconfirmed

1. **Something about our takeover isn't durable/legitimate enough from
   Spotify's point of view**, and by the time of these later test runs
   the phone had already re-established itself as a "real", actively
   verified active session (as opposed to the very first activation of a
   session, seen in an earlier, unlogged-here test, where nobody was
   contesting and the takeover stuck with no fight). Repeated test
   cycles on the same account may have left it in a state where the
   phone is more "sticky" than a fresh account would be.
2. A server-side or app-side timeout independent of anything we can see
   in our own logs, on the order of ~25-40s, that isn't actually reacting
   to anything we did wrong - i.e. this might not be fixable purely
   client-side without knowing what the reference (official) apps do
   differently during that window.

### Next diagnostic steps (if the fix below doesn't fully resolve it)

- **Watch the phone's own Connect device picker live during a test.**
  Does it show the ESP32 as selected/"Connecting..." at all, briefly, or
  never? This tells us whether the rejection is backend-side (server
  never told the phone) or UI/app-side (phone saw it, then reverted).
- **Dump the raw outgoing `PutStateRequest` bytes** for the
  transfer-triggered PUT (hex dump before `httpClient->put()`) and
  diff against a byte-for-byte expected encoding, to rule out anything
  the static review above missed in the actual wire bytes rather than
  the source.
- **Try a completely fresh Spotify account/device** (no history of
  repeated ESP32 test cycles) to check whether hypothesis 1 (account/
  session "stickiness" from repeated tests) holds - if a clean account
  transfers cleanly on the first try, that's a strong signal.
- Consider whether stale "ghost" device registrations from earlier test
  runs (before the HTTP pool fix, when PUTs could silently vanish) are
  still sitting in this account's Connect device list, and whether
  logging out / clearing them changes anything.

### Root cause found: comparing against this repo's own `master` branch

`master` is a separately-written, hardware-proven cspot engine for a
different target (see project memory: don't confuse it with
`feature/esp32-port`'s engine) - but it implements the exact same
Connect-state PUT/activation protocol, so it's a second, *locally
verifiable* reference alongside go-librespot, and one specifically known
to hold onto "active" reliably on real hardware.

Diffing `master`'s `PlayerEngine::sendPutStateRequest`
(`src/PlayerEngine.cpp`) against our `ConnectStateHandler::
flushStateNowLocked` turned up one concrete divergence: `master` sets
`PutStateRequest.message_id` (wire field 6) on *every* PUT, from a
persistent, strictly-incrementing counter:

```cpp
request.message_id = ++messageId;
```

Our port never touched this field at all - it stayed at its default `0`
on every single PUT we ever sent, active or not. go-librespot doesn't set
it either (confirmed in `daemon/player_state.go`), so it wasn't caught by
the go-librespot comparison alone; only diffing against the *other*
proven-working implementation surfaced it.

This lines up with everything observed: every PUT we sent looked
identical/no-newer than the last one from the backend's point of view (a
device claiming activation via message #0, then message #0 again, then
message #0 again...), while the phone's own client presumably increments
a real counter on its PUTs. If Spotify's backend uses `message_id` to
order/arbitrate competing activation claims from the same device
identity, a client that never advances it would plausibly keep losing
that arbitration to a client that does - independent of `is_active`,
independent of pause state, independent of the ~25-40s window (that
window may just be however long the backend waits before reasserting the
last device it considers legitimately active).

**Fix applied, then reverted - tested on real hardware, no change in
behavior.** `ConnectStateHandler` briefly had its own persistent
`messageId` counter, incremented and written into
`putStateRequestProto.messageId` on every actual PUT send, mirroring
`master`'s `++messageId` exactly. Same symptom persisted on hardware
(same ~25-40s reclaim window, active playback). Removed again. Ruled
out as *the* cause, though it may still be worth keeping long-term for
protocol correctness/parity with `master` - just not reinstated here
since it added a variable with no observed effect while this
investigation is ongoing.

**Update, later session: reinstated after all.** Current code
(`ConnectStateHandler.h`'s `nextMessageId` member,
`ConnectStateHandler.cpp:356`,
`putStateRequestProto.messageId = ++nextMessageId;` inside what's now
`prepareAndEncodeLocked()`) increments this on every PUT. No record was
found tying the re-add specifically back to this investigation versus
just general protocol correctness - noted here only so a future reader
doesn't waste time rediscovering that the "removed again" state
described above is stale. Doesn't change the conclusion above (already
ruled out as *the sole* cause via hardware testing before it was first
removed).

## 3. Deeper protobuf wrapper audit: confirmed silent field drops

Prompted by "verifica si estamos haciendo algún filtro silencioso" - yes,
confirmed, though not yet proven to explain this specific bug. This
project hand-writes a `cspot_proto::` wrapper struct + `bindFields()` per
protobuf message on top of nanopb (`main/include/proto/ConnectPb.h`,
`MetadataPb.h`) instead of using nanopb's generated structs directly.
Nanopb only encodes/decodes a callback-typed field if `bindFields()`
explicitly calls `nanopb_helper::bindField()` for it - anything the
wrapper doesn't mention is **silently absent from the wire**, not an
error, not a log line, nothing. `.proto` files themselves were not the
problem (field numbers all check out against go-librespot/`master`) -
the gap is entirely in which of those fields the hand-written wrapper
layer actually bothers to bind.

Diffed every message on the transfer/activation path
(`PlayerState`, `Session`, `Context`, `DeviceInfo`, `Capabilities`,
`Cluster`, `PutStateRequest`, `TransferState`, `Playback`, `Queue`)
field-by-field against their `.proto` definitions and against what
go-librespot's `daemon/player.go` / `daemon/controls.go` /
`daemon/player_state.go` actually populate on the same code paths.

### Found and fixed: `Suppressions` was completely unimplemented

go-librespot **always** sends a non-nil (if empty) `Suppressions` on
every `PlayerState` (`daemon/player_state.go`'s `initState()`:
`Suppressions: &connectpb.Suppressions{}`), and explicitly copies it from
the incoming `TransferState` on every transfer
(`daemon/player.go:236`: `p.state.player.Suppressions =
transferState.CurrentSession.Suppressions`).

This wrapper layer had **no `cspot_proto::Suppressions` struct at all** -
not unbound-by-oversight, structurally absent. `PlayerState.suppressions`
(wire field 18) and `Session.suppressions` (wire field 5) were both
silently dropped: every outgoing PUT omitted the field entirely (rather
than sending it present-but-empty like go always does), and decoding an
incoming `TransferState` silently threw away whatever suppressions data
it carried.

**Fix applied** (`ConnectPb.h`, `ConnectStateHandler.cpp`): added
`cspot_proto::Suppressions` (wraps the one `repeated string providers`
field), wired into both `PlayerState` and `Session`, and
`handleTransferCommand()` now copies
`transferState.current_session.suppressions` into
`playerState.suppressions` exactly like go's `daemon/player.go:236`.
Compiles clean.

**Downgraded confidence after checking `master`**: `master` (the
hardware-proven reference for this exact "stays active" problem) never
sends `Suppressions` at all - zero references anywhere in its source.
It reliably holds "active" without it. This doesn't necessarily mean
`Suppressions` is *wrong* to send (still correct for protocol parity
with go, still harmless), but it's no longer a strong suspect for the
reclaim-after-~30s bug specifically - keeping the fix, but not expecting
it alone to resolve this.

### Checked against `master`, downgraded: `Restrictions`/`ContextRestrictions`

Same shape of gap as `Suppressions` above, bigger message.
`daemon/controls.go:251-252` (track load) and `daemon/player.go:235`
(transfer) both explicitly set:

```go
p.state.player.Restrictions = spotCtx.Restrictions
p.state.player.ContextRestrictions = spotCtx.Restrictions  // and, on transfer, transferState.CurrentSession.Context.Restrictions
```

`PlayerState.restrictions` (field 17), `PlayerState.context_restrictions`
(field 4), and `Context.restrictions` (field 4) are all silently
unbound here - and there's no `cspot_proto::Restrictions` wrapper struct
to bind them to yet. Unlike `Suppressions` (one field), `Restrictions`
has 25 `repeated string disallow_*_reasons` fields plus two
`map<string, ModeRestrictions/RestrictionReasons>` fields - straightforward
but more surface area, and the two map fields need the gap below solved
first.

Left as a follow-up rather than rushed: for a fresh, unrestricted track
(no regional/format restrictions - the common case) every one of these
lists is empty anyway, so its impact on this specific "won't stay active"
bug is unclear; worth adding for general correctness regardless of
whether it turns out to matter here.

**Downgraded after checking `master`**: `master` (the hardware-proven
reference for this exact "stays active" problem) never sets
`player_state.restrictions` (the top-level field, zero references
anywhere in its source), and its only `context_restrictions` usage is
narrow and unrelated to transfer - 3 toggle-repeat/shuffle reason
strings, driven by a separate `update_context` command
(`PlayerCommandHandler.cpp:172-209`/`PlayerStateModel.cpp:112-121`), not
copied from the `TransferState`/context metadata like go-librespot does.
Since `master` stays active reliably without ever populating the
go-style full `Restrictions`/`ContextRestrictions`, this drops to a
low-priority follow-up for protocol completeness rather than a live
suspect for this bug - not implementing it unless new evidence points
back here.

### Found, not yet fixed: `map<string, string>` fields have no binding support at all

`nanopb_helper::bindField()` (`NanoPBHelper.h`) has no case for
`std::map` - only `std::string`, `std::vector<T>`, `std::array<T,N>`,
`Optional<T>`, and plain scalar/enum/struct types. Every
`map<string, string>` (or map-of-message) field in `connect.proto` is
therefore structurally impossible to bind today, not just currently
unbound:

- `PlayerState.context_metadata` (21), `PlayerState.page_metadata` (22)
- `Context.metadata` (3)
- `ContextTrack.metadata` (4), `ProvidedTrack.metadata` (3)
- `ContextPlayerOptions.modes` (5)
- `DeviceInfo.metadata_map` (16), `DeviceInfo.device_aliases` (20)
- `Restrictions.disallow_setting_modes` (28), `Restrictions.disallow_signals` (29)

go-librespot does populate at least `ContextMetadata` unconditionally on
transfer/load (`daemon/player.go:238`, `daemon/controls.go:266-267`: `if
nil { = map[string]string{} }` - same "always present, even if empty"
pattern as `Suppressions`). Adding real map support to
`nanopb_helper.h` (nanopb represents `map<K,V>` as a repeated
`MapFieldEntry{key,value}` submessage under the hood - doable, but a
template on top of the existing `StructCodec`/`bindField` machinery,
not a one-line change) is its own follow-up task, not attempted in this
session.

### Checked and confirmed fine (no gap)

- `DeviceInfo`, `Capabilities`: every field go-librespot sets is bound;
  `Capabilities` isn't message-level `FT_CALLBACK` so its scalar fields
  ride nanopb's normal static encode path anyway (not this wrapper's
  callback machinery) and were never at risk.
- `Cluster.active_device_id`/`.player_state` (the two fields we actually
  read) are correctly bound; the other unbound `Cluster` fields
  (`need_full_player_state`, `server_timestamp_ms`, etc.) were checked
  against go-librespot and are never read there either - not a gap
  relative to the reference, just unused protocol surface.
- `PutStateRequest.only_write_player_state` - unused in go-librespot too,
  not a gap.

## 4. Split the transfer PUT into an early minimal one + the full one (fix applied, pending hardware verification)

Re-read `master`'s `PlayerCommandHandler::handleTransfer` and
go-librespot's `controls.go` (`loadContext()` → `loadCurrentTrackOrSkip()`
→ `loadCurrentTrack()`) more carefully than the first pass. Correction to
what was reported earlier in this doc/conversation: **neither reference
sends anything before resolving the context** - `master`'s
`contextResolver.resolve()` and go's `loadContext()` (which builds
`Track`/`PrevTracks`/`NextTracks`/`Index` from the resolved context) both
run before their first PUT. "PUT before resolving context" was not
actually the distinguishing factor.

What *does* differ: `master`'s first PUT (`putBufferingState()`) carries
only `track.uri` + `position` + `paused` - it does **not** wait to build
`index`/`prevTracks`/`nextTracks` first, unlike go (whose first PUT
already has the full windows, built by `loadContext()` before
`loadCurrentTrack()` is even called) and unlike this branch's previous
behavior (single PUT, after windows were fully built, at the very end of
`handleTransferCommand()`). In both `master` and go, that first
(buffering) PUT goes out *before* the slow part - the audio key/CDN
fetch - begins; only the amount of state included in that first PUT
differs.

**Fix applied** (`ConnectStateHandler.cpp`, `handleTransferCommand()`):
added a new, minimal `putState()` call right after
`trackQueueHandler->loadContext()` resolves and *before*
`setQueue()`/`updateTrackWindows()`, setting only `playerState.track.uri`
(from `trackId`, already computed pre-resolve) on top of everything
already known by that point (`isActive`, `isBuffering`, `isPaused`,
`timestamp`, `contextUri`/`contextUrl`, `options`, `suppressions`,
`sessionId`, `position`, `positionAsOfTimestamp`) - deliberately still
missing `index`/`prevTracks`/`nextTracks`. The existing full PUT (with
those three fields populated) still fires afterward, unchanged, once
`updateTrackWindows()` completes - `putState()`'s existing 200ms
rate-limit coalescing means these two calls typically resolve to two
close-together network PUTs, not a doubling of traffic on every transfer.

Distinguished explicitly (in code comments) from the historical
regression already documented in this file ("An earlier version of this
function sent a bare track.uri... a real hardware regression, client
briefly showed the transfer, then dropped it") - that earlier attempt
apparently sent *only* a bare track URI, with the rest of the request
presumably still default/stale; this version carries every other field
already known at that point, deliberately omitting only the
queue-window fields that genuinely aren't computable yet. Whether this
distinction is actually enough to avoid repeating that regression is
exactly what hardware testing needs to confirm.

Compiles clean. **Not yet verified on real hardware** - watch for the
exact same "flicker" symptom the historical regression comment
describes, not just whether the ~25-40s reclaim window changes.

## 5. Split-PUT verified on hardware: reclaim still happens

Tested the section 4 split-PUT fix on real hardware, alongside a
`Client-Token` header addition to `SpClient::putConnectState()`
(previously the only call in that file missing it) that was applied in
the same working session but never written up here. Neither changed the
outcome: audio genuinely plays (Ogg opens, ranges stream, natural EOF
advances correctly), but the source client still never settles on the
ESP32 as active - now visible as "stuck on Connecting until it gives up"
rather than a silent post-hoc reclaim, same underlying issue.

Confirmed again in a fresh trace: `PUT response cluster: activeDeviceId=
27d006c0b352347e4f8fdbe63ee8cc196d134851` - the same phone device id from
section 2 - shows up as active in *every single* PUT response across an
entire session (9/9), including the very first one, immediately. Also
confirmed (raw byte substring check, `SpClient.cpp:170`) that our own
device *is* listed in the cluster response body every time - we're
registered, just never elected active.

### New data point: Spotify's ~15s transfer retransmit actively rewinds queue position, not just resends

Section 2 (run 2) only noted the ~15s retransmit happens; assumed a
no-op resend. A full trace this session showed it isn't one: the resend
carries the *original* transfer's track/position, and
`handleTransferCommand()`'s unconditional `loadContext()`/`setQueue()`
call re-seeks the local queue back to that stale position even after a
natural EOF had already legitimately advanced past it locally (confirmed
in-log: index 27 -> 28 via natural EOF -> back to 27 via the resend, ~15s
after the first transfer). Two fixes applied this session for the
*local* symptoms this exposed (neither addresses why the phone reclaims):

- `StreamPlayer.cpp`'s EOF handling now clears `currentFile`/
  `currentTrackId` alongside `resetStream()`, fixing a separate ~1s
  spurious reopen-then-immediate-flush of the just-ended track that was
  happening on every natural advance (`maybeStartCurrentTrack()` was
  reopening it from a stale `currentFile` before the real `QUEUE_UPDATED`
  for the next track could arrive) - independent of transfer/reclaim.
- `handleTransferCommand()`/`handlePlayCommand()` now post `PLAYER_FLUSH`
  unconditionally again, removing the "only if track changed" gate from
  section 4-era code - trades an occasional harmless audible restart on a
  genuine duplicate resend for never silently drifting from what the
  latest command says should be playing.

### Standing lead, not yet attempted: `map<string,string>` fields structurally cannot be sent

From section 3: `nanopb_helper::bindField()` has no case for `std::map`
at all. `PlayerState.context_metadata`/`page_metadata` (and
`Context.metadata`) are therefore always fully absent from our PUTs - not
empty-but-present, missing from the wire entirely. go-librespot always
sends these, even as an empty map, on every transfer/load
(`daemon/player.go:238`, `daemon/controls.go:266-267`). This is the only
remaining *structural* (not "we forgot to populate a value", but "the
wrapper layer cannot represent this field at all") gap identified against
go-librespot on this path that hasn't been tried. Real effort (generic
map-as-repeated-`MapFieldEntry` support in `nanopb_helper.h`, not a
one-line change) - not attempted this session.

### Still-untried diagnostics from section 2, still valid

- Fresh Spotify account/device, to rule out this specific account/phone
  pair being "sticky" from many repeated test cycles.
- Watch the phone's own device picker live during a transfer attempt -
  does it show the ESP32 as selected/"Connecting" at all, or never?
- Dump the raw outgoing `PutStateRequest` bytes and diff against a
  byte-for-byte expected encoding.

## 6. Section 4's split-PUT reverted

Single `putState()` call again in `handleTransferCommand()`, after
`updateTrackWindows()` - the early minimal PUT (track.uri only, before the
queue windows were built) added complexity with no observed benefit once
verified on hardware (section 5) and was removed for consistency with
`handlePlayCommand()`/`master`/go-librespot, none of which split a
transfer's PUT into two network round-trips.

## 7. Two more divergences found and fixed, neither yet verified on hardware

### `zeroconf`'s `getInfo` always reported `activeUser: ""`

`targets/cli/main.cpp`'s GET `/spotify_handler?action=getInfo` handler
hardcoded `buildZeroconfJSONResponse(deviceName, deviceId, "")` -
`activeUser` was a literal empty string regardless of whether the device
was actually logged in or actively playing. go-librespot maintains this
live (`zeroconf.go`'s `SetCurrentUser()`, called from `daemon/app.go` on
login/logout) and reflects the real current user on every `getInfo`
response.

Worth investigating specifically for *this* bug because the real Spotify
app is known to cross-check a device's LAN/zeroconf status against its
cloud Connect-state cluster status when corroborating an activation claim
- a device claiming `is_active=true` on the cluster while its own LAN
`getInfo` says no active user is a real, self-inflicted inconsistency
between the two surfaces this firmware exposes, never checked before this
session (all prior sections only looked at the cloud dealer/spclient
path).

**Fix applied**: `activeUser` now reads `authInfo->loginCredentials->username`
live (empty only if genuinely not logged in) instead of a hardcoded `""`.

**Superseded, later session.** This direct-read approach was replaced
with a properly synchronized design: `ZeroconfServer`
(`main/src/ZeroconfServer.cpp`) now owns a mutex-guarded `currentUser_`
member, updated via `setCurrentUser()` from `ConnectReceiver.cpp` at
the specific points it confirms a session is actually running - not
just "credentials exist" - reading `authInfo->loginCredentials` directly
from the GET handler's own thread had no synchronization with the POST
handler's own writes to it. The `getInfo` handler itself also moved
from `targets/cli/main.cpp` to `ZeroconfServer.cpp`. Net behavior for
`activeUser` is unchanged from what's described above (real username
when active, empty when not), just implemented more carefully.

### `device_id` was a deterministic hash of a hardcoded string, not randomly generated

`AuthInfo.h`'s constructor computed `deviceId` as
`deviceIdPrefix + hex(std::hash<std::string>(deviceName))`, where both
`deviceIdPrefix` and `deviceName` (`"Cspot player"`,
`targets/cli/main.cpp`) were hardcoded literals. This is not just
"stable across reboots" (trivially true for a pure function of compile-time
constants) - it means **every build of this firmware, for anyone, gets the
identical device id**, with zero real entropy. (Side finding: the
`std::hash<std::string>` suffix was also silently truncated to 32 bits of
real variation formatted into a 16-hex-digit field, since `size_t` is
32-bit on the ESP32's architecture - the upper 8 hex digits were always
`00000000`.)

go-librespot (`daemon/app.go:94-104`) generates 20 bytes from `crypto/rand`
*once*, then persists it to its state file and reuses it on every
subsequent boot - genuine per-installation entropy plus stability. This
repo already has the persistence half of that (the `session.json` file,
`AuthInfo::toJson()`/`assignDataFromJson()`, which already round-trip
`deviceId`) - it was only ever the *generation* half that had no real
randomness.

**Fix applied**: `AuthInfo`'s constructor now generates 40 random hex
chars (20 bytes of entropy, matching go-librespot's format exactly) via a
seeded `std::random_device`-backed engine, no longer derived from
`deviceName` at all. The existing session-file load path is untouched and
still overwrites this with whatever was persisted from a prior run, so
this generator only actually executes once per device's lifetime (first
ever boot, before `session.json` exists).

**Not yet verified on real hardware** - both fixes are plausible
contributors to "our takeover isn't durable/legitimate enough" (leading
hypothesis 1, section 2), but neither has hardware confirmation yet. If
the reclaim persists after both, the `map<string,string>` gap (above) and
the two still-untried diagnostics remain the next things to try.

## 8. Unrelated but same code path: a `lastTransferTimestamp` guard on `handleClusterUpdate` was added, then found harmful, then removed

Not part of the reclaim investigation above - flagged here only because
it lives in the exact same function (`handleClusterUpdate`/
`stopBeingActive`, section 2) this doc already covers extensively, and a
future reader diffing that function against what's described above
would otherwise be confused by its absence.

At some point after this doc's sections 1-7 were written (no record of
when/why in this file), a staleness guard was added to
`stopBeingActive`: an incoming `ClusterUpdate` showing another device as
active was only honored if
`clusterUpdate.cluster.playerState.timestamp > lastTransferTimestamp` (a
locally-stored value set from the last transfer's own
`TransferState.playback.timestamp`) - intended to filter out a stale/
reordered `ClusterUpdate` right after a legitimate transfer to this
device, matching go-librespot's own identical guard (`daemon/player.go`).

Found and removed in a later session: confirmed on hardware that
reclaiming playback from a PC client sent a `ClusterUpdate` this guard
always rejected. Root cause: `playerState` is an embedded value in this
file's hand-written proto bindings (`Cluster::bindFields`,
`ConnectPb.h`), not a nilable submessage - a server-omitted
`player_state` on this message type is indistinguishable from a genuine
`timestamp=0`, and `0 <= lastTransferTimestamp` is always true. The
guard always saw the update as older than the last transfer and
silently ignored it, leaving the device stuck showing itself as active.
Confirmed go-librespot's own identical guard reads the same field the
same way, so it isn't proof the field is trustworthy - just that this
specific case (a lightweight activation-only `ClusterUpdate` with no
full `player_state`) hasn't been reported there.

Removed entirely - `handleClusterUpdate` now backs off unconditionally
on any `activeDeviceId` mismatch while active, matching `master`'s own
simpler (guard-less) behavior, the same reference this doc's section 2
already established as the hardware-proven baseline for this exact code
path.

## 9. Root cause found and confirmed on hardware: outgoing `ProvidedTrack` dropped `uid`/`provider`

A later session revisited this doc's still-open core symptom from a
different angle: not "why won't the ESP32 stay active" (sections 1-8,
receiving a transfer), but its mirror image, "why does the PC client see
'Spotify can't play this file right now' when the ESP32 cedes control
back" (`handleClusterUpdate`'s `stopBeingActive` → `putInactive()` path).
Same code area, opposite direction of handoff.

### Ruled out first (re-litigating section 8's guard with fresh hardware evidence)

Reproduced the same empty/stale-looking `ClusterUpdate` section 8 already
described (`activeDeviceId=""` or a correctly-populated
`activeDeviceId`, either way paired with `playerState.timestamp=0`) on
two separate captures. Considered reinstating a
`timestamp > lastTransferTimestamp` guard (go-librespot's own
`daemon/player.go:177` formula) - but a capture with a **correctly
populated, non-empty** `activeDeviceId` still had `timestamp=0`, which
would have made that guard reject a legitimate handoff exactly like
section 8's original bug. Confirms section 8's removal was correct;
`playerState.timestamp` on this particular `ClusterUpdate` is not a
trustworthy signal in this client's traffic, full stop - not worth
gating on again.

### Also fixed along the way, not the root cause by itself

- **`TimeProvider`** (`main/include/TimeProvider.h`, new): the ESP32 has
  no RTC battery backup, so its clock is wrong at boot until corrected.
  Added a one-shot NTP query (`bell::net::UDPSocket` to `pool.ntp.org`)
  plus continuous refinement from the AP's own `Ping` packets
  (previously handled ad hoc via `settimeofday()` in `ApClient`, now
  routed through this shared component). Every `system_clock::now()` call
  on the connect-state/playback path now goes through
  `timeProvider->getSyncedTimestamp()`. Fixes real but different
  symptoms (wrong absolute timestamps early in a session) - not the
  "can't play this file" bug itself.
- **`StreamPlayer` transfer-seek race** (`taskLoop()`,
  `handleFlushEvent()`): a transfer's start position was being dropped or
  applied then discarded, in three different ways depending on whether
  the target track was already open, found and fixed iteratively on
  hardware:
  1. A staleness check in `taskLoop()`'s `pendingSeekMs` handling
     dropped a transfer's seek whenever it landed before `currentFile`
     was populated (always true for a cold transfer, given the CDN/audio
     key fetch takes far longer than `taskLoop()`'s own iteration speed).
     Fixed by keying that check off `currentTrackId` (set synchronously)
     instead of `currentFile` (set asynchronously).
  2. Once that was fixed, a *second* race surfaced when the target track
     was **already open** (regaining control of a track this device was
     playing before ceding it): `PLAYER_FLUSH` and `PLAYER_SEEK` were two
     independent posted events, each taking `playbackMutex` separately -
     `taskLoop()` could interleave between them, closing the decoder via
     the flush and immediately reopening it (via
     `maybeStartCurrentTrack()`, which always runs right after a flush)
     before the seek had even arrived, discarding the intended start
     position.
  3. Folded flush, seek, and the transfer's initial `isPlaying` into one
     `FlushResumeState` payload on a single `PLAYER_FLUSH` event
     (`EventModels.h`, `StreamPlayer::handleFlushEvent()`), applied
     atomically under one lock acquisition - closes the race by
     construction instead of trying to order two independent events.
     Also fixed a related bug this same atomicity uncovered: the PC
     client showing the resumed track as *paused* despite the correct
     position, because `maybeStartCurrentTrack()`'s reopen-driven
     announce could fire before a separately-posted `PLAYER_PLAY` had
     updated `StreamPlayer`'s own `isPlaying` member, and
     `maybeStartCurrentTrack()` never re-announces once the decoder is
     already open.

  All three confirmed on hardware via raw `TransferState`/PUT byte
  decodes (same hand-decoding technique as section 3). Real, fixed bugs
  - but the position/pause fixes alone did **not** resolve "can't play
  this file"; the PC still failed to take over cleanly afterward, which
  is what led to root-causing this section.

### Root cause: `ConnectStateHandler` was sending the current track as a bare URI

`TrackQueueHandler::currentTrack()` (`main/src/tracks/TrackQueueHandler.cpp:503-541`)
already computes `uri`, `uid` (e.g. `"q0"` for a queue entry), and
`provider` (`"context"`/`"queue"`) correctly. But in all four places
`ConnectStateHandler` copies that into the outgoing `PlayerState.track`
(`handleTransferCommandLocked`, `handlePlayCommandLocked`, and two more),
it discarded everything except the URI:

```cpp
playerState.track.value = cspot_proto::ProvidedTrack{.uri = track->uri};
```

Compared against go-librespot's `ids.go:71-99`
(`ContextTrackToProvidedTrack()`): it sets `Uri`, `Uid`, `Metadata`,
`ArtistUri`, `AlbumUri`, `Provider` for the *current* track, not just a
bare URI. Spotify identifies a track's position within a context/queue
by `uri`+`uid` together, not `uri` alone - sending `uid=""` on every PUT
meant the PC client, once notified the ESP32 went inactive, could not
reliably re-locate the currently-playing track within the context it was
handed, and gave up with "Spotify can't play this file right now".

**Fix applied** (`ConnectStateHandler.cpp`, all four sites): copy the
whole `*track` instead of reconstructing it from a bare `.uri`. Metadata/
`artistUri`/`albumUri` (which go-librespot also sends, per the proto's
`ProvidedTrack.metadata`/`.artist_uri`/`.album_uri` fields,
`protobuf/connect.proto:67-79`) are still not populated -
`TrackQueueHandler::currentTrack()` doesn't compute them yet - but `uid`/
`provider` alone were enough.

**Confirmed fixed on hardware**: repeated ESP32→PC handoff, previously
reproducing "Spotify can't play this file right now" on every attempt,
now resumes cleanly on the PC client.

### Still open, lower priority

- `metadata`/`artist_uri`/`album_uri` on the outgoing current-track
  `ProvidedTrack` remain unpopulated (see above) - go-librespot sends
  them; harmless gap so far, but not proven harmless, just not the
  thing that was breaking playback.
- The `map<string,string>` binding gap from section 3 is still standing
  and unrelated to this fix.

## 10. PC→ESP32 transfer position fixed; ESP32→PC still starts from 0, deliberately not pursued further

Two more fixes landed in the same later session as section 9, both
confirmed on hardware:

- **Transfer position wasn't extrapolated.**
  `handleTransferCommandLocked()` used to seek to
  `transferState.playback.positionAsOfTimestamp` as-is. That field is a
  snapshot as of `transferState.playback.timestamp` - by the time the
  command is actually processed, that moment can be well in the past
  (confirmed on hardware: over two minutes, once). Fixed by
  extrapolating forward (`positionAsOfTimestamp + elapsed`, matching
  go-librespot's own `daemon/player_state.go` `trackPosition()`, same
  10-minute/negative-elapsed sanity guard) before using it both for the
  actual seek and for what gets sent back out.
- **`update_context` broke the position/timestamp pairing.**
  `handleUpdateContextCommandLocked()` updated `playerState.timestamp`
  to now without touching `positionAsOfTimestamp` - readable as "the
  device jumped back to that frozen position, right now" to a client
  extrapolating the pair, which visibly reset the PC client's displayed
  position to 0 every time it became the foreground window. Fixed by
  not touching either field, matching go-librespot's own handling
  (`daemon/player.go:378-393`), which doesn't touch them either.

### Attempted and reverted: refreshing position before going inactive

By the same reasoning as the two fixes above, tried adding one last
accurate PUT (`positionAsOfTimestamp`/`timestamp` refreshed via
`currentPositionMsLocked()`, the same helper `handlePauseCommandLocked()`
uses) right before `handleClusterUpdate()`'s `stopBeingActive` branch
calls the bodyless `putInactive()` - sent synchronously
(`prepareAndEncodeLocked()` + `putConnectStateRaw()` directly, not the
normal `putStateLocked()` scheduler, since that scheduler needs the same
`putStateMutex` this function holds for its whole body and would only
actually fire *after* `putInactive()` had already gone out).

**Confirmed on hardware: no effect.** The PC client still starts the
handed-off track from 0 even with this fresh, correctly-paired PUT
confirmed sent beforehand. Reverted rather than left in - go-librespot
doesn't do this either (`stopPlayback()` sends nothing but the bodyless
inactive PUT), so whatever mechanism is supposed to carry position across
this specific direction of handoff (ESP32 active → PC taking over) isn't
"send an accurate regular PUT right before going inactive." Possibilities
not yet investigated: the backend may extrapolate server-side from prior
PUTs during active playback rather than reading the final one; or
`/inactive` itself may discard player_state entirely regardless of
timing, in which case no ordering of these two calls could ever help.

**Deliberately not pursued further** - the reverse direction (PC→ESP32,
the two fixes above) is the one confirmed solid, and that was the actual
goal. If this resurfaces, start from PC-side evidence (network inspector
on a Spotify Web Player session, if reproducible there) rather than
another ESP32-side log - every attempt so far has been ESP32-log-only,
and both attempts that touched this exact direction (section 9's original
"can't play this file" and this section's "starts from 0") turned out to
need information this device's own logs alone couldn't provide directly
(section 9 was solved by comparing outgoing wire bytes against
go-librespot's field population, not by reasoning about symptoms).

### Update, later session: intermittent - one passing run, then reproduced again with byte-level evidence

One ESP32→PC handoff test passed (correct resume position) right after
section 11's `PLAYER_FLUSH`-ordering fix, prompting the theory that a
cleaner PC→ESP32 PUT history (no transient wrong-track PUT) might be
helping the backend's resume-state for the reverse direction, even though
no code on the ESP32→PC path itself had changed. **A subsequent run
reproduced the "starts from 0" symptom again**, ruling out "fixed" - it's
intermittent, not resolved.

Decoded the raw outgoing `PutStateRequest` bytes from that failing run
(`position_as_of_timestamp` is proto field 10, `timestamp` is field 1,
`protobuf/connect.proto:35,44`): both fields were byte-identical across
three captures spanning ~2.5 minutes (the PUT right after a natural track
advance, the PUT after an intervening `update_context` command, and the
final in-memory state right before `putInactive()`) -
`position_as_of_timestamp=0`, `timestamp=<the track-advance moment>`.
Confirms two things: `update_context` still correctly leaves both fields
untouched (byte-for-byte identical `PlayerState` before/after it, only
`message_id`/`last_command_message_id` outside the submessage differ) -
not a regression there; and our code genuinely never refreshes this
position anchor during uninterrupted playback of a single track, only at
discrete events (track load/transfer/pause/seek).

**Checked against both reference implementations whether that's a gap:
it isn't.** Neither go-librespot (`daemon/player_state.go`,
`daemon/controls.go`) nor `master` (`PlayerStateModel::setPlaybackState()`,
`PlayerEngine.cpp`) ever refresh `position_as_of_timestamp`/`timestamp`
on a timer independent of a discrete playback event either - every write
site in both traces back to play/pause/seek/track-change/transfer, same
as this branch. `master`'s `runTask()` 500ms wait and go-librespot's
`stateTimer` are both PUT-coalescing throttles armed only after a real
event, not periodic heartbeats. So "we never refresh position on our
own during idle playback" is not a divergence from a proven-working
reference - both hold "active" reliably without doing this either,
which argues against it being *the* cause of this symptom, whatever it
is.

**Net: no code-side bug identified for this specific symptom** by the
comparative method that found every other real bug in this document. The
intermittency (works sometimes, not others, no code change in between)
continues to point toward something outside what ESP32-side logs or a
source diff against these two references can show - same conclusion
section 9/10 already reached, now with one more ruled-out hypothesis.

## 11. Fixed: a transfer briefly played a stale, previously-cached track before correcting itself

### Symptom

PC playing a track, transfer to ESP32: for a couple of seconds the ESP32
played a *different* track (one it had resident from before this
transfer), then self-corrected to the track the PC was actually playing,
at the right position.

### Root cause

`handleTransferCommandLocked()` used to post the combined `PLAYER_FLUSH`
event (position + `isPlaying`, see section 9's `FlushResumeState` fix)
right after computing the extrapolated position - *before*
`trackQueueHandler->clearContext()`/`loadContext()`/`setQueue()`/
`updateTrackWindows()` had run. `loadContext()` is a network fetch
(context/playlist resolution) and can take seconds, or fail outright (seen
on hardware: a transient `Connection reset by peer` failed the first of
two back-to-back identical `TransferState`s - the dealer's own retry).
`StreamPlayer::maybeStartCurrentTrack()` opens whatever `currentFile` it
already has as soon as it sees the flush - if that's still the *previous*
track (because `trackQueueHandler` hasn't been updated by this transfer
yet), it reopens that stale file at the new position instead of waiting
for the real one. A transfer whose `loadContext()` failed would even post
the flush and then return an error - touching playback for a command that
never actually completed.

### Fix applied

Moved the `PLAYER_FLUSH` post to the end of `handleTransferCommandLocked()`
(where a bare `PLAYER_FLUSH`/`PLAYER_PLAY` pair used to live before
section 9's atomicity fix), after `updateTrackWindows()` - so
`trackQueueHandler->currentTrack()` is always resolved to this transfer's
real track before `StreamPlayer` is told to flush. Also means a transfer
that fails before reaching that point (e.g. `loadContext()`'s error
return) never touches `StreamPlayer` at all, matching every other error
path in the function. The combined-event atomicity from section 9 is
unaffected - still one `FlushResumeState` post, just later.

**Confirmed on hardware**: same transfer scenario, no more stale-track
playback before the correction.

## 12. Fixed: outgoing `ProvidedTrack` always sent an empty `uid` for context-sourced tracks (and never sent `artist_uri`/`album_uri`)

### Symptom

Following up on §10's still-open "ESP32→PC handoff sometimes restarts
from 0" with the stale-position-anchor theory ruled out (user tested it
directly: pausing mid-track sends a fresh, correctly-paired
`positionAsOfTimestamp`/`timestamp` PUT via `handlePauseCommandLocked()`,
and the handoff still restarted from 0), decoding the actual outgoing
`PutStateRequest` bytes from a failing run turned up a different, concrete
bug: `PlayerState.track` (and `prev_tracks`/`next_tracks`) always carried
`uid=""` for any track sourced from a loaded context page (playlist/album)
- the overwhelming majority of real playback.

### Root cause

`ContextPageParser` (`main/src/tracks/ContextPageParser.cpp`) did capture
each track's real `uid` from the JSON while parsing (`onString()`) - but
`TrackQueueHandler::onTrackParsed()` only used it transiently, to match
"is this the current track" against the transfer's target, then discarded
it. `FetchedContextPage` only ever stored the track's raw gid (for
reconstructing `uri` later), never `uid`. Every downstream `ProvidedTrack`
construction site that sources from a context page therefore hardcoded
`.uid = ""`: `currentTrack()`, and both branches of
`updateTrackWindows()`. The queue-sourced branches weren't better off
either - they hardcoded a synthetic `"q0"`/`"qN"` even where a real
`ContextTrack.uid` (from `TransferState.queue.tracks`, a genuine wire
field) was sitting right there, unused.

Confirmed via comparison against go-librespot that this is a real
regression, not a shared limitation: it deserializes context pages
straight into its generated proto struct (`spclient/context_resolver.go`),
so `Uid` is never discarded, and `ContextTrackToProvidedTrack()`
(`ids.go:93`) just forwards it. It also forwards `ArtistUri`/`AlbumUri`
(read out of the track's `metadata` JSON object) - our `ContextTrack` had
no fields for those at all, and the parser never attempted to read them.

### Fix applied

- `ContextPageParser`: added a parser state for the track's nested
  `metadata` object, extracting `artist_uri`/`album_uri` (other metadata
  keys, e.g. `decision_id`, stay ignored - nothing downstream needs them).
- `cspot_proto::ContextTrack` gained local-only `artistUri`/`albumUri`
  fields (not bound to the wire, same pattern as its existing `index`
  field) - populated by the parser; empty on a `ContextTrack` decoded from
  `TransferState.queue.tracks` (see scope note below).
- `cspot_proto::ProvidedTrack` gained `artistUri`/`albumUri`, bound to the
  already-existing proto fields `artist_uri`/`album_uri`
  (`protobuf/connect.proto:75,77`, fields 8/10 - no `.proto` changes
  needed).
- `TrackQueueHandler`'s `FetchedContextPage` gained `trackUids`/
  `trackArtistUris`/`trackAlbumUris`, parallel to the existing `trackGids`
  and always pushed together in `onTrackParsed()`. Every `ProvidedTrack`
  construction site that used to hardcode `""` (context branches of
  `currentTrack()`/`updateTrackWindows()`) now reads the real stored
  value; the queue branches now use the real `ContextTrack.uid` when
  present, falling back to the old synthetic value only when it's
  genuinely empty (matching go-librespot's own guard in `addToQueue()`,
  minus its persistent counter - not warranted here, no evidence this
  narrower path caused the symptom being fixed).

**Deliberate scope decision**: this does not implement generic
`map<string,string>` binding in `nanopb_helper` (a separately-tracked,
much bigger gap - §3/§5 above). Sidestepped because `ProvidedTrack.
artist_uri`/`.album_uri` are plain string fields, not part of the
`metadata` map (field 3) - go-librespot itself only ever *reads*
`artist_uri`/`album_uri` out of the map to populate these two dedicated
fields. `ProvidedTrack.metadata` itself, and any `ContextTrack.metadata`
key other than these two, stay unpopulated - unchanged, still-open,
low-priority gap.

### Verified

One hardware run: PC playing a playlist track, transfer to ESP32, played
a few seconds, PC reclaimed - resumed at the correct position instead of
0, and the RAW outgoing `PutStateRequest` showed a real, non-empty `uid`
for `PlayerState.track`. Root-cause connection to §10's symptom is
plausible but not proven by a controlled test (no case was captured with
`uid` present and position still wrong, or vice versa) - the `uid` gap
itself was real and worth fixing regardless. Not yet confirmed across
repeated runs; §10's own single-passing-run note is a reminder this
symptom has looked "fixed" once before and then reproduced again.

## 13. Fixed: clicking a track in the PC client's Queue panel didn't jump to it, just toggled play/pause

### Symptom

Playback active on the ESP32, controlled from the desktop PC client.
Play/pause/next/prev all worked. Clicking a specific track inside the
client's "Queue" panel did not - the ESP32 just toggled play/pause
(or, before a stale-binary retest, silently advanced one track
regardless of which one was clicked).

### Root cause

Spotify Connect's `skip_next` dealer command carries an optional `track`
field naming the exact track to jump to - this is how a Queue-panel click
is implemented client-side, confirmed by tracing go-librespot's git
history to the commit that introduced it (`888787f`, "fix: skipping to
the wrong song in some occasions (#61)", whose own sub-commit messages
say "implement skipping in play queue"). `ConnectStateHandler::
handleSkipNextCommandLocked()` took no arguments at all - the incoming
command JSON, and any `track` field on it, never reached the function.
`TrackQueueHandler::skipToNextTrack()`'s interface already had a
`trackUri` parameter for this, but the implementation ignored it
(`(void)trackUri; //TODO: Implement skipping to specific track in
context`).

### Fix applied

- `handleSkipNextCommandLocked()` now takes the command JSON, extracts an
  optional `track.{uri,uid}`, and forwards it through
  `advanceToNextTrackLocked()` to `TrackQueueHandler::skipToNextTrack()`.
- `skipToNextTrack()` gained a real explicit-target path
  (`skipToTargetTrack()`): it searches `nextTracksWindow` - the same
  `next_tracks` list already being reported to Spotify, so the search
  space can't drift from what the client was actually shown - first
  through the queue-sourced entries, then the context-sourced ones, and
  jumps straight to a match (dropping any manually-queued entries skipped
  over along the way). A target not found in that window (stale client
  state) falls back to resetting the context cursor to its start, mirroring
  go-librespot's own `TrySeek`-failure fallback (`tracks/tracks.go`
  `moveStart`).
- Local control (`requestNext()`, no JSON available) calls
  `advanceToNextTrackLocked()` directly instead of going through the JSON
  half, mirroring how `requestSeek()` already bypasses
  `handleSeekCommandLocked()` in favor of `applySeekLocked()`.
- Deliberate divergence from go-librespot: its own `TrySeek`/
  `ContextTrackComparator` only searches context tracks, never the manual
  queue (`tl.queue`) - clicking a manually-queued item wouldn't resolve
  there at all. This port's search covers both, since the manual queue is
  exactly what's shown at the top of the real Queue panel.

### Verified

Confirmed on hardware (JC3248W535): raw incoming JSON captured via a
temporary debug log matched the assumed shape exactly
(`{"endpoint":"skip_next","track":{"uri":"spotify:track:...","uid":"...",
...}}`), and clicking a track partway down the "coming up next" section
jumped straight to it. The manual-queue branch of `skipToTargetTrack()`
(`provider=="queue"`) was not exercised by this test - the uid captured
was a real context-track uid, not a synthetic `q<N>`, because nothing
could populate the manual queue beyond what a `transfer` provides until
§14 below (`add_to_queue`) shipped.

## 14. Feature: `set_queue` / `add_to_queue` support

### Context

§13's queue-branch (`provider=="queue"` in `skipToTargetTrack()`) had no
real-world way to be exercised: the manual queue only ever contained
whatever a `transfer` provided, because the dealer commands that actually
grow/reorder it client-side - `set_queue` and `add_to_queue` - fell into
`handlePlayerCommand()`'s "unknown command" branch and were dropped.

### Reference (go-librespot, verified via `git log`, not just current source)

- `daemon/controls.go` `AppPlayer.addToQueue(ctx, track)`: assigns a
  synthetic `"q<N>"` uid (incrementing `State.queueID`) only when the
  incoming track has none - "the uid always seems unset, so we have to
  set one manually" - then appends to the queue.
  `AppPlayer.setQueue(ctx, prev, next)`: delegates to
  `tracks.List.SetQueue(prev, next)`; `prev` is accepted on the wire but
  never read.
- `tracks/tracks.go` `List.SetQueue`: keeps the currently-playing queue
  entry in place if already playing from queue, then rebuilds the rest of
  the queue from `next`'s leading run of entries flagged
  `metadata["is_queued"]=="true"` - queued items are always that list's
  prefix, so it stops at the first entry that isn't.
- `daemon/player.go`: on `transfer`, `queueID` resets to the highest
  numeric suffix among any `"q<N>"` uids already in the transferred
  queue, so a later `add_to_queue` can't hand out a colliding uid.
- `tracks/tracks.go` `NextTracks(ctx, nextHint)`: when called from
  `set_queue` with `nextHint=next`, the non-queue tail of the reported
  `next_tracks` is echoed back verbatim from the client-supplied `next`,
  not recomputed from the real context. **Not replicated in this port** -
  `updateTrackWindows()` keeps deterministically recomputing that tail
  from `contextPages`/`contextIndex`. Consequence: if a real client
  reorders/removes items in the "coming up from context" section (not the
  manual-queue section) via `set_queue`, this port won't reflect that -
  only genuine manual-queue add/remove/reorder is honored. Also not
  replicated: invalidating a next-track prefetch (go's `secondaryStream`)
  - this port has no such concept, only intra-track chunk read-ahead (see
  `docs/audio-prefetch-buffer-analysis.md`).

### Design

Same layering as §13: `TrackQueueHandler` gained `addToQueue()` (append)
and `reorderQueue()` (keep the currently-playing entry if applicable,
replace the rest) - two new methods rather than changing the existing
`setQueue()`, which `transfer` still uses with its own, different
contract (always starts from a blank slate). `ConnectStateHandler` owns
JSON parsing, the synthetic-uid counter (`nextManualQueueId`, reset at
`transfer` the same way go resets `queueID`), and orchestration
(`updateTrackWindows()` + `putStateLocked()`) - mirroring go-librespot's
own `AppPlayer`/`tracks.List` split, where the uid counter lives in the
former, not the latter. A `parseTrackRef()` helper (JSON `{uri,uid}` →
`ContextTrack`) is shared by `add_to_queue`, each element of `set_queue`'s
`next_tracks`, and `handleSkipNextCommandLocked()` (retrofitted from its
own inline duplicate).

**Bug avoided, not just a style choice**: both `add_to_queue` and
`set_queue` reject/skip any track with neither `uri` nor `uid`. Without
that check, an empty-identity entry lands in the queue and
`updateTrackWindows()` renders it as a `uri=""` slot in the middle of
`nextTracksWindow` - exactly the sentinel `skipToTargetTrack()` (§13)
reads as "end of populated window". Any real track after that hole would
stop being findable by an explicit `skip_next`, silently breaking §13's
fix for anything queued after the bad entry.

**Known, unreconciled edge case**: `currentTrack()`/`updateTrackWindows()`
already had their own positional `"q0"`/`"q<index>"` fallbacks for a
queue track with no uid (predating this feature), independent of the new
`nextManualQueueId` counter. A transfer bringing in an empty-uid queue
entry could in theory display a uid that a later `add_to_queue` also
hands out. Pre-existing, not introduced here, not resolved here.

### Verified

Build only so far (CLI); hardware verification pending - see the plan's
own verification steps (add tracks via "Add to queue", click one
partway down the list, confirm §13's queue-branch now actually engages;
drag-reorder within the Queue panel to exercise `set_queue`).

## 15. Resolved (different cause than suspected): Android/PC show a stale ~18-day-old `Cluster.playerState.timestamp` while the ESP32 itself plays correctly

### Symptom

Reported on hardware: PC transfers playback to the ESP32, ESP32 plays an
80s-playlist track correctly (confirmed by its own logs and audio), but
opening the Android Spotify client at that point shows a *different*,
unrelated Classical track as "now playing" - one that was never
transferred or played in this session.

### Ruled out

- **Not our outgoing PUT.** The last `PutStateRequest` sent right before
  the symptom was decoded byte-for-byte with `protoc --decode_raw`
  (`main/src/ConnectStateHandler.cpp`'s own wire format, cross-checked
  against `protobuf/connect.proto`): correct current track, `is_active=
  true`, `PlayerState.timestamp`/`started_playing_at`/
  `client_side_timestamp` all fresh and mutually consistent with the
  track actually playing.
- **Not a `ConnectPb.h` field-binding mismatch.** `PlayerState.timestamp`
  is field 1 in both the standalone `PlayerState` message and the one
  embedded in `Cluster`, and the hand-written binding
  (`ConnectPb.h::PlayerState::bindFields`) maps it by name to the
  nanopb-generated struct field, which nanopb itself binds to the
  correct wire number - no drift between struct declaration order and
  wire order.
- **Not §8's absent-`player_state`-decodes-as-zero bug.** The
  `CLUSTER DIAG` log line (`ConnectStateHandler.cpp:510`) shows
  `playerState.hasValue=true` on both observed occurrences - Spotify
  really did send a populated `player_state` submessage, this isn't the
  defaulted-`Optional` case §8 hit.

### Evidence

Two separate incoming `ClusterUpdate` messages, 84 seconds apart, both
with several successful `Put state succeeded` (fresh, correctly
advancing timestamps) in between, echoed back the exact same
`playerState.timestamp=1787799985089` - a value ~18.4 days *ahead* of
the real session time (`1786207223580` at the same moment). The field
is frozen across messages despite our own successful writes landing in
between - server-side staleness, not a decode artifact.

### Leading hypothesis, unconfirmed

At some earlier session (timing unknown - no logs from then), this
persisted `device_id` sent a `PutStateRequest` with a skewed/incorrect
`PlayerState.timestamp`. Spotify's backend cached it as the "latest"
state for this device under a last-writer-wins-by-timestamp scheme.
Every subsequent legitimate PUT (correct, but numerically *lower*
timestamp) loses that comparison server-side, so the cluster state
visible to other clients - and echoed back to us in `ClusterUpdate` -
never advances past the poisoned entry.

### `TimeProvider` gap found; hardening attempted, then reverted

`TimeProvider` (`main/src/TimeProvider.cpp`) has no go-librespot
equivalent to diff against - go-librespot has no NTP or AP-ping clock
sync at all, it trusts the host OS's already-synced system clock
(confirmed: its own `PacketTypePing` handler only echoes a Pong, never
reads the payload). This subsystem is unique to the embedded port.

Two weaknesses found in it:
- `queryNtp()` runs once at boot, no retry on failure.
- `syncWithPingPacket()` (refines the clock roughly every ~2 minutes
  from the AP's own ping payload) applies any 4-byte value
  unconditionally, with no plausibility check against the
  already-established offset - a single corrupted or misparsed ping
  could silently skew the clock for an entire session, producing
  exactly this class of correct-looking-but-wrong timestamp with
  nothing to catch it at the time.

A guard against the second weakness (`hasSynced` flag +
`kMaxPlausibleResyncJumpMs`, rejecting any resync implying a jump over 5
minutes except the very first sync of a boot) was implemented and
compiled, then deliberately reverted before committing - once the real
cause turned out to be a Spotify-side cache (see the resolution note at
the end of this section), the clock-side hardening was judged
unnecessary. Only the readable-date logging added alongside it
(`formatEpochMs`, in the "Synced time via NTP"/"Time offset refined from
AP ping" log lines) stayed; current code applies every resync
unconditionally. Both weaknesses above remain unaddressed - don't
reintroduce the guard unless asked again.

### Still open

- Root cause of the *original* poisoning event (if that's really what
  this is) is unconfirmed - no logs exist from whenever it happened.
- Planned next diagnostic, not yet run as of this writing: close both
  the PC and Android Spotify clients, reopen only Android, and capture
  what `ConnectStateHandler` logs at that moment (particularly any new
  `CLUSTER DIAG`) to see whether the stale value ever gets displaced.
- Possibly related to a separately-tracked, still-open investigation
  into a doubly hex-encoded context-track `uid` also producing
  stale-looking cluster/position state after reconnecting - both are
  "Spotify shows something old/wrong to another client while the ESP32
  itself is correct," but not confirmed to share a root cause.

### Resolved, later session - different cause than suspected

The frozen-timestamp symptom this section chased turned out to have a
different root cause than the leading hypothesis above: two
`ClusterUpdate` messages 84 seconds apart (with successful, correctly-
advancing `PutStateRequest`s in between) echoed back the exact same
`playerState.timestamp`, ~18 days ahead of the real session clock -
consistent with a Spotify-backend cache keyed on the persisted
`device_id`, stuck on a stale/skewed entry that no subsequent correct
PUT could displace by timestamp comparison. Erasing flash (which
resets `device_id`) made the symptom disappear, confirming the cache
theory over a firmware-side timestamp bug.

The double-hex-encoded `uid` theory mentioned above was never formally
confirmed or ruled out as a contributor to a related-but-distinct
symptom (an impossible playback position, e.g. 33:52 in a 7:03 track)
- it simply stopped being pursued once the cluster/track-identity
symptom driving this investigation had a confirmed explanation. If an
impossible-position symptom resurfaces, that theory is still open and
untested, not disproven.
