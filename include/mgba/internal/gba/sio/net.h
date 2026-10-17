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
	GGA_NET_RTC_SEED = 0x27
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

/* Little-endian scalar helpers (unaligned-safe) */
void GGANetStoreU16(void* dest, uint16_t value);
void GGANetStoreU32(void* dest, uint32_t value);
void GGANetStoreI32(void* dest, int32_t value);
uint16_t GGANetLoadU16(const void* src);
uint32_t GGANetLoadU32(const void* src);
int32_t GGANetLoadI32(const void* src);

CXX_GUARD_END

#endif
