# GGA Netplay — network link driver (Phase 1 design)

## Where it plugs in

`src/gba/sio/lockstep.c` already implements deterministic 4-player link
(`playerId 0-3`, coordinator + attached players, desync asserts, versioned
savestates). The Qt `MultiplayerController` attaches multiple *local* cores
to one coordinator. Network play keeps that core untouched and adds:

- One **host** device runs the `GBASIOLockstepCoordinator` (player 0).
- Each remote device runs its own `GBASIOLockstepDriver` + local core.
- Remote players appear in the host coordinator as proxy entries; the
  coordinator's wait/ack/wake barriers and per-player event queues are the
  exact points bridged over TCP. No changes to `sio.c` timing.

New code lives next to lockstep (same conventions, MPL header — see
License section), e.g. `src/gba/sio/net.c` + `include/.../sio/net.h`.

## Wire protocol (TCP, little-endian, v1)

Framing: `[u8 id][u16 le len][payload]`. All multi-byte integers LE.

| id | Name | Dir | Payload |
|----|------|-----|---------|
| 0x10 | HELLO | C->H | proto u8 (=1), core_ver u32, rom_hash (SHA-256, 32B), name_len u8, name |
| 0x11 | WELCOME | H->C | player_id u8, start_cycle i32 |
| 0x12 | REJECT | H->C | reason u8 (1=proto 2=core 3=rom 4=full) |
| 0x20 | TRANSFER | both | cycle i32, slot u8, data u16 |
| 0x21 | EVENT | both | type u8 (attach/detach/mode/hardsync), player u8, arg i32 |
| 0x22 | STATE_REQ | C->H | (empty; asks host for full state) |
| 0x23 | STATE | H->C | len u32 + versioned savestate bytes |
| 0x06 | HEARTBEAT | both | cycle i32 |
| 0x07 | BYE | both | reason u8 |

Rules:

- Same core version **and** same ROM hash, or `REJECT` with a visible
  reason. No silent mismatches.
- Lockstep barrier per multiplayer transfer: the host relays each slot's
  `u16` to all parties; a party that misses its deadline repeats its
  last value with a stall counter; persistent stall or failed assert ->
  host pushes `STATE` (full resync, same mechanism as savestates).
- Late join: handshake + `STATE` push, then live transfers.
- Cap 4 players (`player_id` 0-3, host is 0). The byte range reserves
  headroom without complicating v1.

## Why this shape

- Transfers are game-driven (tens-to-hundreds/sec, 2 bytes each):
  trivial bandwidth on LAN; latency budget is the barrier wait.
- Full-state resync reuses the existing versioned savestate path, so
  join/recovery needs no second mechanism.
- Determinism is inherited: identical core + identical transfer stream
  = identical state. The existing desync asserts become our test oracle.

## Test plan (no ROMs in repo; user supplies their dumps)

1. Loopback: 2× `mgba-sdl` on one PC, host + client, commercial 2P title.
2. LAN: PC + phone (once the Android shell exists) or 2 PCs.
3. 4P: 4 devices on 4P titles (Mario Kart, Advance Wars, Four Swords…).
4. Soak: 30-min session with periodic state-hash comparison (v1.1).

## License

New in-tree netplay files keep the MPL-2.0 header like their neighbors.
Standalone tooling outside `src/` may use other licenses per README.
