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

- [ ] Input-sync driver skeleton (N local cores, local input, no network)
- [ ] Startup blob exchange (ROM + save + deterministic RTC seed)
- [ ] Host tick authority + guest tick buffer + backpressure
- [ ] Desync detection (per-player hashes, stop-on-mismatch)
- [ ] Detach + session lifecycle; then 4P

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
