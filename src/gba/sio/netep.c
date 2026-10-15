/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/netep.h>

#include <mgba/internal/gba/sio/net.h>
#include <mgba/internal/gba/sio/netdriver.h>

#include <stdlib.h>
#include <string.h>

static void _transportSend(void* context, int targetPlayer, int kind, int32_t a, int32_t b,
	const void* data, size_t len);

void GBASIONetEndpointInit(struct GBASIONetEndpoint* ep, struct GBASIONetLink* link,
	const struct GBASIONetTransport* frontend) {
	memset(ep, 0, sizeof(*ep));
	ep->link = link;
	ep->listenSock = INVALID_SOCKET;
	ep->conn = INVALID_SOCKET;
	if (frontend) {
		link->transport = *frontend;
	}
	link->transport.context = ep;
	link->transport.send = _transportSend;
}

void GBASIONetEndpointSetIdentity(struct GBASIONetEndpoint* ep, const uint8_t romHash[32], const char* name) {
	memcpy(ep->romHash, romHash, 32);
	strncpy(ep->name, name, sizeof(ep->name) - 1);
}

void GBASIONetEndpointClose(struct GBASIONetEndpoint* ep) {
	if (!SOCKET_FAILED(ep->conn)) {
		GGANetSendBye(ep->conn, 0);
		SocketClose(ep->conn);
		ep->conn = INVALID_SOCKET;
	}
	if (!SOCKET_FAILED(ep->listenSock)) {
		SocketClose(ep->listenSock);
		ep->listenSock = INVALID_SOCKET;
	}
	free(ep->outbox);
	ep->outbox = 0;
	ep->outboxLen = ep->outboxCap = 0;
	free(ep->inReasm);
	ep->inReasm = 0;
	ep->inReasmTotal = ep->inReasmGot = 0;
}

bool GBASIONetEndpointListen(struct GBASIONetEndpoint* ep, int port) {
	SocketSubsystemInit();
	ep->listenSock = SocketOpenTCP(port, 0);
	if (SOCKET_FAILED(ep->listenSock)) {
		return false;
	}
	if (SOCKET_FAILED(SocketListen(ep->listenSock, 4))) {
		SocketClose(ep->listenSock);
		ep->listenSock = INVALID_SOCKET;
		return false;
	}
	SocketSetBlocking(ep->listenSock, false);
	ep->isHost = true;
	return true;
}

bool GBASIONetEndpointConnect(struct GBASIONetEndpoint* ep, const char* host, int port) {
	SocketSubsystemInit();
	struct Address addr;
	memset(&addr, 0, sizeof(addr));
	if (SocketResolveHost(host, &addr)) {
		return false;
	}
	ep->conn = SocketConnectTCP(port, &addr);
	if (SOCKET_FAILED(ep->conn)) {
		return false;
	}
	SocketSetBlocking(ep->conn, false);
	SocketSetTCPPush(ep->conn, 1);
	ep->isHost = false;
	uint8_t payload[1 + 4 + 1 + 32];
	size_t at = 0;
	payload[at++] = GGA_NET_PROTO;
	GGANetStoreU32(&payload[at], GGA_NET_CORE_VERSION);
	at += 4;
	payload[at++] = 0;
	memcpy(&payload[at], ep->romHash, 32);
	at += 32;
	size_t nl = strlen(ep->name);
	if (nl > 200) {
		nl = 200;
	}
	/* name appended raw after fixed part */
	uint8_t* full = malloc(at + nl);
	memcpy(full, payload, at);
	memcpy(full + at, ep->name, nl);
	bool ok = GGANetSendFrame(ep->conn, 0x10, full, (uint16_t) (at + nl));
	free(full);
	return ok;
}

static bool _outboxAppend(struct GBASIONetEndpoint* ep, uint8_t id, const void* payload, uint16_t len) {
	size_t need = ep->outboxLen + 3 + len;
	if (need > ep->outboxCap) {
		size_t cap = ep->outboxCap ? ep->outboxCap * 2 : 4096;
		while (cap < need) {
			cap *= 2;
		}
		uint8_t* grown = realloc(ep->outbox, cap);
		if (!grown) {
			return false;
		}
		ep->outbox = grown;
		ep->outboxCap = cap;
	}
	ep->outbox[ep->outboxLen++] = id;
	GGANetStoreU16(&ep->outbox[ep->outboxLen], len);
	ep->outboxLen += 2;
	if (len) {
		memcpy(&ep->outbox[ep->outboxLen], payload, len);
		ep->outboxLen += len;
	}
	return true;
}

static void _transportSend(void* context, int targetPlayer, int kind, int32_t a, int32_t b,
	const void* data, size_t len) {
	struct GBASIONetEndpoint* ep = context;
	(void) targetPlayer;
	(void) b;
	/* Map netdriver kinds to wire ids (docs/NETPLAY.md). */
	uint8_t id = 0;
	uint8_t fixed[32];
	const void* pay = data;
	size_t payLen = len;
	uint8_t tmp[40];
	switch (kind) {
	case 1: /* TRANSFER_START */
		id = 0x20;
		GGANetStoreI32(tmp, a);
		pay = tmp;
		payLen = 4;
		break;
	case 2: /* MODE_SET */
		id = 0x23;
		tmp[0] = 2;
		tmp[1] = 0;
		GGANetStoreI32(&tmp[2], a);
		pay = tmp;
		payLen = 6;
		break;
	case 3: /* HARD_SYNC */
		id = 0x23;
		tmp[0] = 3;
		tmp[1] = 0;
		GGANetStoreI32(&tmp[2], 0);
		pay = tmp;
		payLen = 6;
		break;
	case 4: /* ATTACH */
		id = 0x23;
		tmp[0] = 4;
		tmp[1] = 0;
		GGANetStoreI32(&tmp[2], 0);
		pay = tmp;
		payLen = 6;
		break;
	case 5: /* DETACH */
		id = 0x23;
		tmp[0] = 5;
		tmp[1] = 0;
		GGANetStoreI32(&tmp[2], 0);
		pay = tmp;
		payLen = 6;
		break;
	case 6: /* RESULT */
		id = 0x22;
		if (len > sizeof(tmp)) {
			return;
		}
		memcpy(tmp, data, len);
		pay = tmp;
		break;
	case 0: /* TRANSFER_DATA */
		id = 0x21;
		break;
	case 7: /* ACK */
		id = 0x23;
		tmp[0] = 7;
		tmp[1] = (uint8_t) a;
		GGANetStoreI32(&tmp[2], 0);
		pay = tmp;
		payLen = 6;
		break;
	default:
		return;
	}
	(void) fixed;
	_outboxAppend(ep, id, pay, (uint16_t) payLen);
}

static void _flushOutbox(struct GBASIONetEndpoint* ep) {
	while (ep->outboxLen > 0 && !SOCKET_FAILED(ep->conn)) {
		ssize_t n = SocketSend(ep->conn, ep->outbox, ep->outboxLen);
		if (n <= 0) {
			if (!SocketWouldBlock()) {
				SocketClose(ep->conn);
				ep->conn = INVALID_SOCKET;
			}
			break;
		}
		memmove(ep->outbox, &ep->outbox[n], ep->outboxLen - (size_t) n);
		ep->outboxLen -= (size_t) n;
	}
}

static void _dispatch(struct GBASIONetEndpoint* ep, uint8_t id, const uint8_t* pay, uint16_t len) {
	struct GBASIONetLink* link = ep->link;
	switch (id) {
	case 0x10: { /* HELLO */
		if (!ep->isHost || len < 38) {
			break;
		}
		uint8_t proto = pay[0];
		uint32_t core = GGANetLoadU32(&pay[1]);
		if (proto != GGA_NET_PROTO) {
			GGANetSendReject(ep->conn, 1);
	} else if (core != GGA_NET_CORE_VERSION) {
			GGANetSendReject(ep->conn, 2);
		} else if (len < 38 || memcmp(&pay[6], ep->romHash, 32) != 0) {
			GGANetSendReject(ep->conn, 3);
		} else {
			uint8_t w[5] = { 1, 0, 0, 0, 0 };
			GGANetStoreI32(&w[1], 0);
			GGANetSendFrame(ep->conn, 0x11, w, sizeof(w));
		}
		break;
	}
	case 0x11: /* WELCOME */
		if (!ep->isHost && len >= 5) {
			link->assignedSlot = pay[0];
			ep->welcomed = true;
		}
		break;
	case 0x12: /* REJECT */
		ep->peerBye = true;
		break;
	case 0x20: /* TRANSFER_START */
		if (len >= 4) {
			int32_t fc = GGANetLoadI32(pay);
			GBASIONetLinkOnWire(link, 0, 1, fc, 0, 0, 0);
		}
		break;
	case 0x21: /* TRANSFER_DATA */
		if (len >= 5) {
			uint32_t v = GGANetLoadU32(&pay[1]);
			GBASIONetLinkOnTransferData(link, pay[0], v);
		}
		break;
	case 0x22: /* TRANSFER_RESULT */
		if (len >= 17) {
			GBASIONetLinkOnWire(link, 0, 6, 0, 0, pay, len);
		}
		break;
	case 0x23: /* EVENT */
		if (len >= 6) {
			int ev = pay[0];
			int pl = pay[1];
			int32_t arg = GGANetLoadI32(&pay[2]);
			if (ev == 7) {
				/* ACK addressed by player in `pl`. */
				GBASIONetLinkOnWire(link, pl, 7, pl, 0, 0, 0);
			} else {
				GBASIONetLinkOnWire(link, 0, ev, arg, 0, 0, 0);
			}
		}
		break;
	case 0x24: /* STATE_REQ */
		if (ep->isHost && link->transport.saveState) {
			void* bytes = 0;
			size_t size = 0;
			if (link->transport.saveState(link->transport.context, &bytes, &size)) {
				size_t off = 0;
				while (off < size) {
					size_t chunk = size - off;
					if (chunk > GGA_NET_CHUNK) {
						chunk = GGA_NET_CHUNK;
					}
					uint8_t* frame = malloc(chunk + 8);
					GGANetStoreU32(frame, (uint32_t) size);
					GGANetStoreU32(frame + 4, (uint32_t) off);
					memcpy(frame + 8, (uint8_t*) bytes + off, chunk);
					_outboxAppend(ep, 0x25, frame, (uint16_t) (chunk + 8));
					free(frame);
					off += chunk;
				}
				free(bytes);
			}
		}
		break;
	case 0x25: { /* STATE chunk */
		if (len < 8) {
			break;
		}
		uint32_t total = GGANetLoadU32(pay);
		uint32_t off = GGANetLoadU32(pay + 4);
		if (total > 8 * 1024 * 1024) {
			break;
		}
		if (!ep->inReasm || ep->inReasmTotal != total) {
			free(ep->inReasm);
			ep->inReasm = malloc(total);
			ep->inReasmTotal = total;
			ep->inReasmGot = 0;
		}
		size_t n = len - 8;
		if (off + n > total) {
			break;
		}
		memcpy(ep->inReasm + off, pay + 8, n);
		if (off + n > ep->inReasmGot) {
			ep->inReasmGot = off + n;
		}
		if (ep->inReasmGot >= total && link->transport.loadState) {
			link->transport.loadState(link->transport.context, ep->inReasm, total);
			free(ep->inReasm);
			ep->inReasm = 0;
			ep->inReasmTotal = ep->inReasmGot = 0;
		}
		break;
	}
	case 0x06:
	case 0x07:
		if (id == 0x07) {
			ep->peerBye = true;
		}
		break;
	default:
		break;
	}
}

void GBASIONetEndpointPoll(struct GBASIONetEndpoint* ep) {
	if (!SOCKET_FAILED(ep->listenSock) && SOCKET_FAILED(ep->conn)) {
		Socket peer = SocketAccept(ep->listenSock, 0);
		if (!SOCKET_FAILED(peer)) {
			SocketSetBlocking(peer, false);
			SocketSetTCPPush(peer, 1);
			ep->conn = peer;
		}
	}
	if (!SOCKET_FAILED(ep->conn)) {
		for (;;) {
			if (ep->inPayLen == 0) {
				while (ep->hdrGot < sizeof(ep->hdrAcc)) {
					ssize_t n = SocketRecv(ep->conn,
						&ep->hdrAcc[ep->hdrGot], sizeof(ep->hdrAcc) - ep->hdrGot);
					if (n == 0) {
						goto peer_gone;
					}
					if (n < 0) {
						if (!SocketWouldBlock()) {
							goto peer_gone;
						}
						goto drain_done;
					}
					ep->hdrGot += (size_t) n;
				}
				uint8_t hdr[3];
				memcpy(hdr, ep->hdrAcc, sizeof(hdr));
				ep->hdrGot = 0;
				ep->inId = hdr[0];
				uint16_t len = GGANetLoadU16(&hdr[1]);
				if (len > GGA_NET_MAX_PAYLOAD) {
					goto peer_gone;
				}
				ep->inPayLen = len;
				ep->inPayGot = 0;
				ep->inId = hdr[0];
			}
			while (ep->inPayGot < ep->inPayLen) {
				ssize_t n = SocketRecv(ep->conn, ep->inbuf + ep->inPayGot,
					ep->inPayLen - ep->inPayGot);
				if (n == 0) {
					goto peer_gone;
				}
				if (n < 0) {
					if (!SocketWouldBlock()) {
						goto peer_gone;
					}
					goto drain_done;
				}
				ep->inPayGot += (size_t) n;
			}
			_dispatch(ep, ep->inId, ep->inbuf, ep->inPayLen);
			ep->inPayLen = ep->inPayGot = 0;
		}
	drain_done:
		_flushOutbox(ep);
	}
	goto polled;
peer_gone:
	SocketClose(ep->conn);
	ep->conn = INVALID_SOCKET;
	_flushOutbox(ep);
polled:
	GBASIONetLinkPoll(ep->link);
}
