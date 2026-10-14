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

enum GGANetMsg {
	GGA_NET_HEARTBEAT = 0x06,
	GGA_NET_BYE = 0x07,
	GGA_NET_HELLO = 0x10,
	GGA_NET_WELCOME = 0x11,
	GGA_NET_REJECT = 0x12,
	GGA_NET_TRANSFER = 0x20,
	GGA_NET_EVENT = 0x21,
	GGA_NET_STATE_REQ = 0x22,
	GGA_NET_STATE = 0x23
};

enum GGANetRejectReason {
	GGA_NET_REJECT_PROTO = 1,
	GGA_NET_REJECT_CORE = 2,
	GGA_NET_REJECT_ROM = 3,
	GGA_NET_REJECT_FULL = 4
};

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

/* Little-endian scalar helpers (unaligned-safe) */
void GGANetStoreU16(void* dest, uint16_t value);
void GGANetStoreU32(void* dest, uint32_t value);
void GGANetStoreI32(void* dest, int32_t value);
uint16_t GGANetLoadU16(const void* src);
uint32_t GGANetLoadU32(const void* src);
int32_t GGANetLoadI32(const void* src);

CXX_GUARD_END

#endif
