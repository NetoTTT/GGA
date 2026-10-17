/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_NET_H
#define GBA_SIO_NET_H

#include <mgba-util/common.h>
#include <mgba-util/socket.h>

CXX_GUARD_START

/* GGA netplay v1: framing [u8 id][u16 le len][payload]. See docs/NETPLAY.md */
#define GGA_NET_PORT_DEFAULT 27182
#define GGA_NET_PROTO 1
#define GGA_NET_CORE_VERSION 1
#define GGA_NET_MAX_PAYLOAD 0xFFFF

/* Wire IDs. These mirror the live transport in src/gba/sio/netep.c; keep
 * them in sync (netep.c is the implementation of record). */
enum GGANetMsg {
	GGA_NET_HEARTBEAT = 0x06,
	GGA_NET_BYE = 0x07,
	GGA_NET_HELLO = 0x10,
	GGA_NET_WELCOME = 0x11,
	GGA_NET_REJECT = 0x12,
	GGA_NET_TRANSFER_START = 0x20,
	GGA_NET_TRANSFER_DATA = 0x21,
	GGA_NET_TRANSFER_RESULT = 0x22,
	GGA_NET_EVENT = 0x23,
	GGA_NET_STATE_REQ = 0x24,
	GGA_NET_STATE = 0x25,
	GGA_NET_BLOB = 0x26,
	GGA_NET_RTC_SEED = 0x27,
	/* v2 input netplay (docs/NETPLAY-ARCHITECTURE.md): the host owns the
	 * frame tick, guests feed it their input, and both sides compare
	 * per-player state hashes. */
	GGA_NET_INPUT = 0x28,
	GGA_NET_TICK = 0x29,
	GGA_NET_HASH = 0x2A,
	GGA_NET_PAUSE = 0x2B,
	GGA_NET_RESUME = 0x2C,
	GGA_NET_STOP = 0x2D,
	GGA_NET_DETACH = 0x2E
};

enum GGANetRejectReason {
	GGA_NET_REJECT_PROTO = 1,
	GGA_NET_REJECT_CORE = 2,
	GGA_NET_REJECT_ROM = 3,
	GGA_NET_REJECT_FULL = 4
};

/* Startup blob kinds (v2 input netplay, docs/NETPLAY-ARCHITECTURE.md). */
enum GGANetBlobKind {
	GGA_NET_BLOB_ROM = 0,
	GGA_NET_BLOB_SAVE = 1
};

/* GGA_NET_BLOB payload: [u8 kind][u32 total][u32 offset][u32 hash] then
 * chunk bytes. Chunks may arrive in any order and are placed by offset;
 * hash is a 32-bit FNV-1a over the whole blob so a torn or truncated
 * transfer is rejected instead of silently desyncing the session. */
#define GGA_NET_BLOB_HEADER 13
#define GGA_NET_BLOB_CHUNK 60000

/* Blocking helpers. recvFrame waits up to timeoutMs (< 0 = forever).
 * Returns false on error/timeout/oversize. outLen is payload length. */
bool GGANetSendFrame(Socket sock, uint8_t id, const void* payload, uint16_t len);
bool GGANetRecvFrame(Socket sock, uint8_t* id, uint8_t* payload, uint16_t* len, int64_t timeoutMs);

bool GGANetSendHello(Socket sock, uint8_t proto, uint32_t coreVer, const uint8_t romHash[32], const char* name);
bool GGANetSendWelcome(Socket sock, uint8_t playerId, int32_t startCycle);
bool GGANetSendReject(Socket sock, uint8_t reason);
bool GGANetSendTransfer(Socket sock, int32_t cycle, uint8_t slot, uint16_t data);
bool GGANetSendEvent(Socket sock, uint8_t type, uint8_t player, int32_t arg);
bool GGANetSendHeartbeat(Socket sock, int32_t cycle);
bool GGANetSendBye(Socket sock, uint8_t reason);

/* FNV-1a over a byte range; used for blob integrity. */
uint32_t GGANetHash32(const void* data, size_t len);

/* Startup blob sharing (ROM + in-game save). SendBlob chunks internally;
 * RecvBlob reassembles, verifies the hash and hands back a malloc'd
 * buffer the caller frees. RecvBlob consumes only GGA_NET_BLOB frames
 * and returns false on any other id, timeout, or corruption. */
bool GGANetSendBlobChunk(Socket sock, uint8_t kind, uint32_t total, uint32_t offset,
	uint32_t hash, const void* data, uint16_t len);
bool GGANetSendBlob(Socket sock, uint8_t kind, const void* data, size_t len);
bool GGANetRecvBlob(Socket sock, uint8_t* kind, void** dataOut, size_t* lenOut, int64_t timeoutMs);

/* Deterministic RTC seed, applied before reset so every client starts
 * from the same hidden epoch. Value is milliseconds since the Unix
 * epoch, sent as two little-endian u32. */
bool GGANetSendRtcSeed(Socket sock, int64_t epochMs);
bool GGANetRecvRtcSeed(Socket sock, int64_t* epochMs, int64_t timeoutMs);

/* v2 tick protocol payloads. Keys are a 16-bit active-high mask in the
 * GBA key order (A, B, Select, Start, Right, Left, Up, Down, R, L).
 *   INPUT  [u32 frame][u16 keys]                    guest -> host
 *   TICK   [u32 frame][u16 keys x nPlayers]         host  -> all
 *   HASH   [u32 frame][u8 nPlayers][u32 hash x n]   both ways
 * PAUSE/RESUME/STOP carry [u8 reason]; DETACH carries [u8 slot][u8 reason]
 * and are sent with GGANetSendFrame directly.
 *
 * Calls that take a keys/hashes array expect room for GGA_NET_MAX_PLAYERS
 * entries; RecvTick/RecvHash report how many the frame actually held.
 * Sends reject a player count outside [1, GGA_NET_MAX_PLAYERS]; receives
 * reject a frame whose length does not match its player count. */
#define GGA_NET_MAX_PLAYERS 4
#define GGA_NET_INPUT_SIZE 6
#define GGA_NET_HASH_HEADER 5
bool GGANetSendInput(Socket sock, uint32_t frame, uint16_t keys);
bool GGANetRecvInput(Socket sock, uint32_t* frame, uint16_t* keys, int64_t timeoutMs);
bool GGANetSendTick(Socket sock, uint32_t frame, const uint16_t* keys, uint8_t nPlayers);
bool GGANetRecvTick(Socket sock, uint32_t* frame, uint16_t* keys, uint8_t* nPlayers, int64_t timeoutMs);
bool GGANetSendHash(Socket sock, uint32_t frame, const uint32_t* hashes, uint8_t nPlayers);
bool GGANetRecvHash(Socket sock, uint32_t* frame, uint32_t* hashes, uint8_t* nPlayers, int64_t timeoutMs);

/* Little-endian scalar helpers (unaligned-safe) */
void GGANetStoreU16(void* dest, uint16_t value);
void GGANetStoreU32(void* dest, uint32_t value);
void GGANetStoreI32(void* dest, int32_t value);
uint16_t GGANetLoadU16(const void* src);
uint32_t GGANetLoadU32(const void* src);
int32_t GGANetLoadI32(const void* src);

CXX_GUARD_END

#endif
