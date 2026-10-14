/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/net.h>

#include <string.h>

void GGANetStoreU16(void* dest, uint16_t value) {
	uint8_t* d = dest;
	d[0] = value & 0xFF;
	d[1] = (value >> 8) & 0xFF;
}

void GGANetStoreU32(void* dest, uint32_t value) {
	uint8_t* d = dest;
	d[0] = value & 0xFF;
	d[1] = (value >> 8) & 0xFF;
	d[2] = (value >> 16) & 0xFF;
	d[3] = (value >> 24) & 0xFF;
}

void GGANetStoreI32(void* dest, int32_t value) {
	GGANetStoreU32(dest, (uint32_t) value);
}

uint16_t GGANetLoadU16(const void* src) {
	const uint8_t* s = src;
	return (uint16_t) s[0] | ((uint16_t) s[1] << 8);
}

uint32_t GGANetLoadU32(const void* src) {
	const uint8_t* s = src;
	return (uint32_t) s[0] | ((uint32_t) s[1] << 8) |
		((uint32_t) s[2] << 16) | ((uint32_t) s[3] << 24);
}

int32_t GGANetLoadI32(const void* src) {
	return (int32_t) GGANetLoadU32(src);
}

static bool _sendAll(Socket sock, const void* buffer, size_t size) {
	const uint8_t* p = buffer;
	while (size > 0) {
		ssize_t n = SocketSend(sock, p, size);
		if (n <= 0) {
			return false;
		}
		p += n;
		size -= (size_t) n;
	}
	return true;
}

static bool _recvAll(Socket sock, void* buffer, size_t size, int64_t timeoutMs) {
	uint8_t* p = buffer;
	while (size > 0) {
		if (timeoutMs >= 0) {
			Socket reads[2] = { sock, INVALID_SOCKET };
			if (SocketPoll(2, reads, 0, 0, timeoutMs) <= 0) {
				return false;
			}
		}
		ssize_t n = SocketRecv(sock, p, size);
		if (n <= 0) {
			return false;
		}
		p += n;
		size -= (size_t) n;
	}
	return true;
}

bool GGANetSendFrame(Socket sock, uint8_t id, const void* payload, uint16_t len) {
	uint8_t header[3];
	header[0] = id;
	GGANetStoreU16(&header[1], len);
	if (!_sendAll(sock, header, sizeof(header))) {
		return false;
	}
	if (len > 0) {
		return _sendAll(sock, payload, len);
	}
	return true;
}

bool GGANetRecvFrame(Socket sock, uint8_t* id, uint8_t* payload, uint16_t* len, int64_t timeoutMs) {
	uint8_t header[3];
	if (!_recvAll(sock, header, sizeof(header), timeoutMs)) {
		return false;
	}
	uint16_t want = GGANetLoadU16(&header[1]);
	/* want is bounded by the u16 length field itself (<= MAX_PAYLOAD). */
	if (!payload || !len) {
		return false;
	}
	if (want > 0 && !_recvAll(sock, payload, want, timeoutMs)) {
		return false;
	}
	*id = header[0];
	*len = want;
	return true;
}

bool GGANetSendHello(Socket sock, uint8_t proto, uint32_t coreVer, const uint8_t romHash[32], const char* name) {
	size_t nameLen = strlen(name);
	if (nameLen > 255) {
		nameLen = 255;
	}
	uint8_t payload[1 + 4 + 1 + 32 + 255];
	size_t at = 0;
	payload[at++] = proto;
	GGANetStoreU32(&payload[at], coreVer);
	at += 4;
	payload[at++] = (uint8_t) nameLen;
	memcpy(&payload[at], romHash, 32);
	at += 32;
	memcpy(&payload[at], name, nameLen);
	at += nameLen;
	return GGANetSendFrame(sock, GGA_NET_HELLO, payload, (uint16_t) at);
}

bool GGANetSendWelcome(Socket sock, uint8_t playerId, int32_t startCycle) {
	uint8_t payload[5];
	payload[0] = playerId;
	GGANetStoreI32(&payload[1], startCycle);
	return GGANetSendFrame(sock, GGA_NET_WELCOME, payload, sizeof(payload));
}

bool GGANetSendReject(Socket sock, uint8_t reason) {
	return GGANetSendFrame(sock, GGA_NET_REJECT, &reason, 1);
}

bool GGANetSendTransfer(Socket sock, int32_t cycle, uint8_t slot, uint16_t data) {
	uint8_t payload[7];
	GGANetStoreI32(&payload[0], cycle);
	payload[4] = slot;
	GGANetStoreU16(&payload[5], data);
	return GGANetSendFrame(sock, GGA_NET_TRANSFER, payload, sizeof(payload));
}

bool GGANetSendEvent(Socket sock, uint8_t type, uint8_t player, int32_t arg) {
	uint8_t payload[6];
	payload[0] = type;
	payload[1] = player;
	GGANetStoreI32(&payload[2], arg);
	return GGANetSendFrame(sock, GGA_NET_EVENT, payload, sizeof(payload));
}

bool GGANetSendHeartbeat(Socket sock, int32_t cycle) {
	uint8_t payload[4];
	GGANetStoreI32(payload, cycle);
	return GGANetSendFrame(sock, GGA_NET_HEARTBEAT, payload, sizeof(payload));
}

bool GGANetSendBye(Socket sock, uint8_t reason) {
	return GGANetSendFrame(sock, GGA_NET_BYE, &reason, 1);
}
