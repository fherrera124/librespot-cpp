# Connect-state activation: investigation notes

Working notes from debugging why the ESP32 receiver doesn't reliably take
over playback via Spotify Connect (`transfer` command → PUT
`is_active=true` → the source device is supposed to hand off control).
Kept here so a future session doesn't have to re-derive all of this from
scratch. Two issues are covered: one fixed, one still open.

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
