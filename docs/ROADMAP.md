# GGA Roadmap

## Phase 0 — Scaffold (current)

- [x] Public repo with full imported history
- [x] README + license posture (MPL-2.0 for derived files, MIT for new)
- [ ] `gga/` directory with first netplay skeleton

## Phase 1 — Network link driver

- [ ] `GBASIODriver` implementation over TCP (modeled on `src/gba/sio/lockstep.c`)
- [ ] Wire protocol: handshake (versions + ROM hash), per-transfer 16-bit
      payloads, desync detection and resync
- [ ] Host election + late join over full-state transfer

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
