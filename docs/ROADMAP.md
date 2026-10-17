# GGA Roadmap

## Phase 0 — Scaffold (current)

- [x] Public repo with full imported history
- [x] README + license posture (MPL-2.0 for derived files, MIT for new)
- [ ] `gga/` directory with first netplay skeleton

## Phase 1 — Network link driver (v1: transport-forwarded SIO) [superseded]

> Sync model superseded by **Phase 1c — input netplay** (decision:
> `docs/NETPLAY-ARCHITECTURE.md`). Transport + handshake below are reused.

- [x] TCP transport (`src/gba/sio/net.c`): framing, handshake
      (proto/core/ROM hash), per-transfer payloads
- [x] Lockstep bridge (`netdriver.c` + proxy hooks): 2-process
      host+client reach full frame target over TCP loopback
      (`gga/link-netharness2.c`)
- [ ] Wire protocol: desync detection and resync, late join over
      full-state transfer
- [ ] Host election for >2 players

### Phase 1b — LAN UI (planned, Qt frontend)

- [ ] New "LAN" tab/dialog: Host button (port field, default 27182,
      slot list 0-3) and Join button (host IP/hostname + port fields)
- [ ] Connection status per slot (connecting/ready/dropped), start
      session only when all attached
- [ ] No-account design: plain TCP works over LAN and over VPN
      (ZeroTier/Tailscale/WireGuard) for distance play with no relay
- [ ] Later: lobby hub (Phase 3) — room list, join-by-code, open-host
      directory; internet relay stays a separate track after LAN is solid

### Phase 1c — Deterministic input netplay (current target)

See `docs/NETPLAY-ARCHITECTURE.md`. Every client runs all linked cores
locally; the network carries startup blobs, per-frame inputs, and state
hashes (host-authoritative ticks).

- [x] Input-sync driver skeleton (N local cores, local input, no network):
      `gga/link-session.c` runs two cores plus one lockstep coordinator in a
      single thread, drives them a frame at a time with local input, and proves
      local determinism (identical state hashes across runs). `--siotest` also
      drives a real MULTI exchange, so the data path and its drain are covered,
      not just the park/ack barrier.
- [x] Startup blob exchange (ROM + save + deterministic RTC seed):
      `GGANetSendBlob`/`GGANetRecvBlob` chunk with a whole-blob hash, and
      `netep.c` ships the RTC seed plus the ROM and save right after WELCOME
      (new `GBASIONetTransport` blob/RTC callbacks). Covered by
      `gga/net-selftest.c` and `gga/ep-blob-test.c`. The Qt LAN tab supplies
      the RTC seed pair (`CoreController::transportGetRtcSeed` /
      `transportLoadRtcSeed`, backed by a shared `mRTCSource` so both cores
      read one clock); the ROM/save blobs are still unwired there.
- [x] Host tick authority + guest tick buffer + backpressure:
      2-player `netsession.c` in the core: the host resolves one key mask per
      player per frame, the guest runs a tick only when it arrives and answers
      with its keys `GGA_NET_INPUT_DELAY` frames ahead, and a stalled guest
      caps the host at `GGA_NET_TICK_AHEAD` frames instead of letting it run
      away. Covered by `gga/netsession-test.c` (keys land on the exact frame,
      host capped at frame 9 with the guest stalled). 4P and the Qt multi-core
      view are still open.
- [x] Desync detection (per-player hashes, stop-on-mismatch):
      `GBASIONetSessionFrameDone` samples every `GGA_NET_HASH_INTERVAL` frames
      and each side compares both copies as they arrive, whichever lands
      second; a mismatch sets `desync` and stops the session without resyncing.
      The same test compares 130 clean frames and then trips the host on a
      deliberately corrupted guest hash.
- [x] Detach + session lifecycle:
      `GBASIONetSessionEnded`/`EndReason` report why a session stopped
      (local stop, peer `STOP`/`BYE`/`REJECT`, dead socket, desync, protocol
      error), and `netep.c` feeds the last three back from the wire. A
      departing peer ends a 2-player session; `NextFrame` stays false after.
      Covered by `gga/netsession-test.c`, which kills the guest both ways
      (graceful `BYE` -> `PEER_BYE`, bare socket close -> `PEER_GONE`).
- [x] Local lockstep runner for v2 sessions (`netrunner.c`):
      `GBASIONetRunner` owns N cores and advances them a frame at a time with
      the coordinator's cooperative park/wake, driven by an optional
      `GBASIONetSession` (its key masks when set, a fixed mask otherwise) and
      reporting real `saveState` hashes on the session's sampling frames.
      Covered by `gga/netrunner-test.c`: two cores stay in lockstep through a
      real MULTI exchange and hash the same across runs, then two independent
      clients - each running both players' cores - talk over TCP loopback and
      match their real hashes clean.
- [x] Qt frontend hosts the runner (`CoreController::attachLAN2`): a second
      GBA core is built from the running ROM/BIOS image (`VFileFromConstMemory`)
      and both cores are stepped by the runner from the existing 5 ms LAN
      worker, paced at ~60 fps. `mCoreThread` is paused for the duration (the
      runner owns the local core) and its pause is hidden from the UI, which
      keeps `isPaused()`/`pause` coherent via `m_lan2Paused`. The peer core is
      headless (dummy renderer) and the local core still feeds the single
      `Display`. RTC is a shared fixed-epoch source so both cores read the same
      time. Driven by the "Input sync (v2)" checkbox in the LAN dialog; v1 stays
      the default and the fallback. Compile-verified only - the two-machine
      behavior still needs a manual run.
- [ ] ROM/save blobs over v2 (the peer is mirrored from the local image for
      now, so a mismatched-opponent flow and save transfer are not wired)
- [ ] Second view for the peer core (one view is enough to play 2P today)
- [ ] 4P + crossplay (PC/mobile) + soak

## Phase 2 — Android frontend

- [ ] SDL-based mobile shell (touch controls, audio/video backends)
- [ ] Battery/performance budget for real-time link play

## Phase 3 — Lobby hub

- [ ] LAN discovery (UDP broadcast), room list, join-by-code
- [ ] No-account LAN play; identity only where internet relay needs it

## Phase 4 — 4-player beta + crossplay

- [ ] 4-device LAN beta on commercial 4P titles
- [ ] Crossplay between platforms (PC <-> mobile, same core + same
      protocol; matrix: Windows/Linux x86_64 <-> Android arm64)
- [ ] Mixed-lobby validation: 2 PC + 2 phones in one session
- [ ] Internet relay (separate track, after LAN is solid)

## Non-goals for now

Commercial-accuracy timing beyond what keeps 4P titles playable,
anti-cheat for public servers, iOS port.
