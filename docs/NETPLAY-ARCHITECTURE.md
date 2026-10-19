# GGA Netplay Architecture

Status: **accepted** (2026-10-17) · Supersedes the sync model in
`docs/NETPLAY.md` (v1). Project language is English.

## TL;DR

GGA netplay pivots from **transport-forwarded SIO** (v1 — relay each GBA
cable transfer over the wire) to **deterministic input netplay** (v2 —
every client runs the whole linked set of GBA cores locally and only
exchanges per-frame inputs).

v1 puts a network round-trip inside *every* cable transfer, so the link
speed is bounded by latency (measured: ~56 ms/transfer on LAN). v2 keeps
the cable local (mGBA's own lockstep drives it deterministically) and
moves the network wait to input delivery. This is the approach the only
production GBA-cable-over-internet service (Rebit) ships.

## Context

### What v1 was

- One host process runs `GBASIOLockstepCoordinator` (player 0); each
  remote runs its own `GBASIOLockstepDriver` + local core.
- `src/gba/sio/netdriver.c` bridges the coordinator's wait/ack/wake
  barriers and per-player event queues over TCP (`netep.c` transport).
- Every multiplayer transfer is a host↔remote round-trip: host
  `START`/`RESULT`, remote `DATA`.

### What we measured (2 Qt processes, TCP loopback)

| Metric | Value |
| --- | --- |
| Host transfers/s | ~17.8 (≈56 ms per round-trip) |
| LAN pump interval | 5 ms (≈200 Hz) |
| Host parks / transfers | 8588 / 3954 |
| Client parks | 0 (free-runs) |
| Host view of peer slot | `0000`/oscillating → P2 never registers |
| Client view | P2 connects/disconnects in a loop; ~1–2 fps visual (60 fps counter) |

The round-trip sits in the critical path: the transfer cannot complete
until the peer's bytes cross the network, and we measured ~11 pump ticks
per transfer. TCP_NODELAY is already set (`netep.c`), so this is latency
and coordination, not Nagle.

## Evidence (ecosystem)

| System | Model | Result |
| --- | --- | --- |
| **Rebit** (rebit.cc) | input netplay, all cores local | ships 2P/3P/4P link over the internet |
| **VBA-M** (`gbaLink.cpp`) | forward SIO over socket | works, widely reported slow/unstable |
| **mgba-switch-netlink** | forward SIO over TCP | 2P trades on localhost; cross-machine pending |
| **mGBA** `lockstep.c` | local deterministic lockstep | upstream foundation we reuse |

### Rebit — input netplay (the production reference)

> "Do not make the internet pretend to be a cable. Make every browser
> simulate the whole cable locally."

Each browser runs every linked core (2–4 GBAs) plus the shared SIO
lockstep coordinator. Over the network it sends only: each player's
ROM/save, per-frame button masks, session start/pause/stop, and periodic
state hashes. The host is authoritative and sends ordered `tick` frames.
Key lessons from their post-mortem:

- **Determinism is a checklist**, not a vibe: same ROM, save, emulator
  build, config, RTC, link timing, execution order, transfer drain,
  player assignment, and post-reset state.
- **RTC is a hidden input.** Host seeds a deterministic RTC epoch; every
  player applies the same value before reset and subsequent reads agree.
- **Transfer drain.** Cores can finish a visible frame while link work is
  still queued; the frame runner must drain pending transfers (bounded
  assist passes) before returning to the UI. Trade/save boundaries are
  the sensitive case.
- **Hashes + honesty.** Every 60 frames, compare per-player state hashes;
  on mismatch *stop* (link sessions may be trading/saving — hiding a bad
  state is worse than stopping).
- **Host authority / backpressure.** Guests wait for ordered ticks; the
  host limits how far ahead it runs based on acks; jitter is buffered.
- **Detach.** A player leaving should "unplug the cable" and let the rest
  continue when the game allows.

### mgba-switch-netlink — the forward model, done carefully

This is the same model as our v1 and it *does* work for menu-paced games
(FireRed/LeafGreen trades, 2 consecutive enter/trade/exit flows, 2 Qt
processes on one macOS machine). Worth studying, but its own README scopes
it to **2 players, same LAN, localhost-validated** and notes TCP "does not
solve emulated clock synchronization". Its fixes are only relevant to v1:

- **MODE epoch**: a persistent TCP session is separate from the in-game
  MULTI epoch; both peers must be in a fresh MULTI epoch before "Cable
  Ready"; leaving MULTI clears Busy/pending/clock/pacing/Cable Ready
  without closing TCP.
- **Terminal START race**: if a final host `START` crosses the join's exit
  from MULTI, the join returns `DATA=FFFF` (absent slave) so the host
  clears Busy and delivers its IRQ instead of hanging forever.
- **Host freezes emulated time at the transfer boundary** while waiting
  for the peer (short, socket-wakeable waits). Advancing game time during
  the wait makes Pokémon FireRed/LeafGreen hit a comms timeout.
- **Immediate `SIOMULTI0`**: the master word is visible in `SIOMULTI0` at
  transfer start; unavailable slots stay `FFFF`; only local word +
  completion are committed at the end of the hardware period.
- **Clock pacing**: reset both `linkTime` and its `lastCycle` origin on
  `START`; join keeps excess cycles so an ahead-limiter constrains drift.

### mGBA #3421 and GBATEK

- mGBA #3421: hardware returns `0xFFFF` for a slot whose slave supplied no
  fresh data; mGBA historically repeats the slave's last value.
- GBATEK, *SIO Multi-Player Mode*: `SIOMULTI0-3` reset to `FFFF` on
  transfer start; after a transfer all connected GBAs hold the *same*
  values; the parent sends blindly; ID bits are valid only after the first
  transfer; a missing child causes a timeout.

## Why v1 stalls

For action games (e.g. Sonic Battle) a link transfer happens many times
per frame-paced step. If each transfer costs a full network round-trip,
the emulated link clock can only advance at the round-trip rate — the
game runs at ~1/N fps regardless of CPU. v1 can be made *correct* (see
switch-netlink) for slow, menu-driven games, but it cannot be made *fast*
over a real link. GGA's goal is general 4P crossplay, so we choose the
model that removes the round-trip from the cable.

## Decision

Adopt **v2: deterministic input netplay**.

- Every client runs the **whole linked set** of GBA cores (P1..P4) plus one
  local `GBASIOLockstepCoordinator`. The cable is simulated locally.
- The network carries only: startup data, per-frame inputs, session
  control, and periodic state hashes.
- The host is authoritative for the frame tick.

This keeps the emulated cable off the network, which is the only way to
reach full speed on real links, and it scales 2P→4P without a new
networking model (a lesson Rebit calls out explicitly).

## v2 design

### Runtime shape

```
per client:
  core P1 ─┐
  core P2 ─┼─ one local GBASIOLockstepCoordinator ─► frames 1..N
  core P3 ─┤        (all cable timing stays here)
  core P4 ─┘
  input router  (host ticks in, local input out)
  hash probe    (per-player state hash every 60 frames)
```

### Data exchanged

| Message | Dir | Contents |
| --- | --- | --- |
| HELLO / WELCOME / REJECT | both | proto, core build, ROM hash |
| BLOB (ROM + save) | both | per-player ROM + in-game save, chunked |
| RTC_SEED | H→all | deterministic epoch applied before reset |
| INPUT | G→H | `frame`, the guest's own `keys`; doubles as an ack |
| TICK | H→all | `frame`, authoritative `keys[players]` for that frame |
| HASH | both | `frame`, per-player state hash |
| PAUSE / RESUME / STOP | H→all | session control |
| DETACH | both | player left; keep session alive if safe |
| BYE | both | reason |

Startup blob sharing has a privacy cost (players exchange saves) — keep
it explicit in the UI, and never log or persist blobs.

### Determinism checklist (must hold or we stop)

- same core build (version handshake) and same ROM hash;
- same in-game save per player;
- same deterministic RTC seed and RTC reads;
- same emulated execution order and **transfer drain** behavior;
- same player assignment / role mapping;
- same post-reset state;
- same core construction for every player (mirrors built by the core,
  not by the frontend -- see below).

### Mirror core construction

Every player's core runs locally, but a frontend only builds the one the
user sees. The extra players are mirrors, and building them by hand in
the frontend is a determinism hazard: two cores with byte-identical
serialized state can still diverge when their non-serialized wiring
differs.

Measured (Zelda FSA, two Qt processes, TCP loopback, diagnostic build):

- on the last frame before the game opens the link port, the two cores
  in one process have byte-identical serialized state (`stateSize` =
  397312), identical `siocnt`/`rcnt`, identical lockstep player fields
  and identical `cpu` state;
- the next frame diverged in 2350 bytes across 329 runs. The divergence
  was traced to the cart save: the mirror had been built without one, so
  when the game probed the EEPROM (DMA3 read at `0x0D000000`) the local
  core returned the real save bits and the mirror returned an erased
  `0xFF` pattern;
- execution order, slave park/wake and the re-anchored slave clock were
  ruled out as causes by reordering the step loop and disabling the
  park; none moved the divergence frame.

Root cause: at session start the local core's save is still in
`GBA_SAVEDATA_AUTODETECT`, so its data buffer has not been mapped yet
(`savedata.data == NULL`) even though the underlying `VFile` is set. The
original guard copied the save only when `data` was non-NULL, so the
mirror was created with no save at all. The helper now also clones the
save from `savedata.vf` when the buffer is not mapped, so both cores
start from identical save content. The mirror's copy is private: writes
the game makes in-session stay in the mirror and are not written back to
the user's save file.

Decision: mirrors are built by the core, never by the frontend.

```c
struct mCore* GBASIONetRunnerCreateMirrorCore(struct mCore* prototype,
	mColor* buffer, unsigned width);
```

The helper lives in `src/gba/sio/netrunner.c` and copies platform, BIOS,
ROM, save, every config value, `opts` and the `mPERIPH_ROTATION` /
`mPERIPH_GBA_LUMINANCE` / `mPERIPH_RUMBLE` peripherals from the running
core, then resets. The video buffer is optional and stays owned by the
caller. Frontends (Qt today; Android, SDL and iOS later) call this for
players 2..N, so no platform code carries core construction of its own.

With the save cloned, two processes playing Zelda FSA stay in sync: the
per-frame state hashes match through the MULTI handshake (the first
checkpoint after it, frame 119, agrees across all four core instances)
and the run reaches its frame target with no `DESYNC`.

Not covered yet:

- config *overrides* (`mCoreConfigEnumerate` walks values, not
  overrides), e.g. `savePlayerId`;
- the rest of the per-core runtime wiring the emulation can observe:
  `setAVStream`, `keyCallback`, `coreCallbacks`, `sync` and the frontend
  thread. They are mirrored or reset where they mattered here, but a
  future frontend must keep doing so.

### Tick protocol (concrete)

Keys are a 16-bit active-high mask per player. Every device steps all
linked cores together for one frame with the keys the router holds for
that frame number, so the tick is the only thing that orders execution.

Host:

- frame F needs keys for every player: its own from local input, each
  guest's from `INPUT(F)`.
- the host emits `TICK(F)` once it holds every key for F, or when the
  input deadline for F passes; a missing key becomes the idle mask
  `0x0000` and the session is flagged as lagging rather than stalled.
- the host never runs further ahead than `GGA_NET_TICK_AHEAD` (8) frames
  of its slowest guest. `INPUT(F)` acks every tick below F, so the cap is
  cheap: stop emitting F until `min(guestAck) >= F - TICK_AHEAD`.

Guest:

- buffers received ticks in order and runs frame F only when `TICK(F)`
  has arrived, then immediately answers with `INPUT(F + 1)` — one message
  both feeds the host and acks progress, so no separate ack is needed.
- a duplicate tick below its current frame, or a gap it cannot fill,
  fails the session.

PAUSE/RESUME/STOP are session control from the host; a guest that has
stopped keeps draining until `STOP` arrives so the shutdown is ordered.

### Transfer drain

After running the players' frames, keep running for a bounded number of
assist passes while the coordinator still has an active/pending transfer,
respecting sleeping players and a max-loop guard. This is what makes the
in-game link menu survive real game behavior (Rebit's "demo boots" →
"link survives" step).

### Detach

Removing a player detaches its SIO driver, wakes the rest, and keeps the
session alive when the game tolerates a disconnected cable.

For two players there is nobody left to keep running, so a departure ends
the session. `GBASIONetSessionOnPeerGone` records why, first reason wins,
and `NextFrame` goes false for good once ended:

| reason | source |
| --- | --- |
| `GGA_NET_END_LOCAL` | we called `Stop` |
| `GGA_NET_END_PEER_STOP` | the peer's `STOP` |
| `GGA_NET_END_PEER_BYE` | the peer's `BYE` |
| `GGA_NET_END_REJECT` | the peer's `REJECT` |
| `GGA_NET_END_PEER_GONE` | the peer's socket died (EOF/reset) |
| `GGA_NET_END_DESYNC` | hashes disagreed |
| `GGA_NET_END_PROTOCOL` | a frame we could not place |

Keeping the remaining players running needs three or more, and lands with
4P support (`DETACH` on the wire).

### Desync policy

Compare per-player hashes every 60 frames (`GGA_NET_HASH_INTERVAL`). On
mismatch: **stop and warn**; do not silently resync a session that may be
mid-trade/mid-save. Optional "stop, export state, restart" flow later.

### Test harness (control server)

The LAN path is scripted through an in-process control server rather than
a human at the UI. `LanControlServer` (`src/platform/qt/`) listens on the
port named by `GGA_CTL_PORT` and speaks one-line JSON commands over TCP:

- `emu.load <rom>`, `emu.run`, `emu.quit`;
- `lan.host <port>`, `lan.join <host> <port>`;
- `emu.screenshot <path>` (PNG/screenshot helper);
- `lan.status` -> `lan_v2_active`, `lan_v2_hosting`, `lan_v2_running`,
  `lan_v2_frame`, `lan_v2_error`;
- `state.dump <slot> <path>` -> writes that slot's serialized state
  (`GBASIONetRunnerDumpNow`) for a cross-machine byte diff.

`gga/lan-autotest.ps1` drives two instances (host and guest) over the
control ports, waits for a frame count, then copies each process's
`lanplay-<pid>.log`. Set `GGA_DIAG=1` in the environment to make the
`DBG v2` session, hash and park/ack lines print; with it unset a normal
run is quiet.

## What we reuse vs rebuild

Reuse:
- `netep.c` TCP transport, framing, handshake, ROM-hash check;
- the Qt **LAN tab** (Host/Join, port 27182, slot list);
- mGBA's local `lockstep.c` and its savestate path;
- `mCoreThreadWaitFromThread` / harness patterns.

Rebuild:
- `netdriver.c` (SIO↔wire bridge) becomes an **input-sync driver**;
- the wire protocol gains BLOB / RTC_SEED / TICK / HASH;
- the frontend grows a multi-core host view (N cores + N framebuffers).

## Option B (fallback): keep v1 for menu-paced games

If a quick 2P "trades/battles over VPN" release is needed before v2 lands,
cherry-pick the switch-netlink fixes into v1 (MODE epoch, terminal-START
`FFFF`, host freeze at boundary, immediate `SIOMULTI0`, clock pacing). It
will still not reach full speed for action games. Keep this as an
insurance path, not the target.

## Phases

1. **Docs + decision** (this file). No code.
2. **Input-sync driver skeleton**: run N cores locally, drive from local
   input, no network — prove local determinism + drain + hashes.
3. **Startup blob exchange** (ROM + save + RTC seed) over `netep.c`.
4. **Host tick authority** + guest tick buffer + backpressure.
5. **Desync detection** (per-player hashes, stop-on-mismatch).
6. **Detach** + session lifecycle.
7. **4P** + crossplay (PC↔mobile) + soak.

## Risks / open questions

- **CPU/memory**: N cores per client is heavy on mobile (Rebit pays this
  too). Budget and profile early.
- **Save privacy**: startup blobs share saves; design the consent UX.
- **Determinism surface**: RTC, config, build, and reset must all match;
  a single mismatch desyncs the whole room.
- **Input latency**: unavoidable (input→host→guests); trades/menus
  tolerate it, fast action needs small buffers tuned per connection.
- **Transfer drain** correctness on trade/save boundaries.
- **Mirror wiring**: mirror construction is centralized, but a mirror
  whose serialized state matches the local core can still run
  differently if its non-serialized wiring (AV stream, key callback,
  callbacks, sync) differs. The save comment above was one such case;
  the check is "same frame, same player, no byte difference" on a
  handshake frame, and `state.dump` on the control server produces the
  bytes to compare.

## References

- Rebit blog — *Making the GBA Link Cable Work over the Internet*:
  https://daudau.cc/posts/making-gba-link-cable-work-over-the-internet
- Rebit manual — GBA Link Cable: https://rebit.cc/docs/gba-link-cable
- mgba-switch-netlink (forward model, fixes):
  https://github.com/senhaoran213/mgba-switch-netlink
- VBA-M link (`gbaLink.cpp`):
  https://github.com/visualboyadvance-m/visualboyadvance-m/blob/master/src/core/gba/gbaLink.cpp
- mGBA issue #3421: https://github.com/mgba-emu/mgba/issues/3421
- GBATEK — SIO Multi-Player Mode:
  https://problemkaputt.de/gbatek-sio-multi-player-mode.htm
