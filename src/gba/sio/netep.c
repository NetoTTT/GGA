/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/netep.h>

#include <mgba/core/log.h>
#include <mgba/internal/gba/sio/net.h>
#include <mgba/internal/gba/sio/netdriver.h>
#include <mgba/internal/gba/sio/netsession.h>

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
	free(ep->blobReasm);
	ep->blobReasm = 0;
	ep->blobActive = false;
	ep->blobTotal = ep->blobGot = 0;
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

bool GBASIONetEndpointSend(struct GBASIONetEndpoint* ep, uint8_t id, const void* payload, uint16_t len) {
	return _outboxAppend(ep, id, payload, len);
}

/* Queue a whole startup blob as GGA_NET_BLOB chunks. Mirrors the STATE
 * chunking shape, adding the kind byte and a whole-blob hash. */
static bool _queueBlob(struct GBASIONetEndpoint* ep, uint8_t kind, const void* bytes, size_t size) {
	uint32_t hash = GGANetHash32(bytes, size);
	uint8_t* frame = malloc(GGA_NET_BLOB_HEADER + GGA_NET_BLOB_CHUNK);
	uint32_t off = 0;
	bool ok = true;
	if (!frame) {
		return false;
	}
	do {
		uint16_t n;
		if (size - off > GGA_NET_BLOB_CHUNK) {
			n = GGA_NET_BLOB_CHUNK;
		} else {
			n = (uint16_t) (size - off);
		}
		frame[0] = kind;
		GGANetStoreU32(&frame[1], (uint32_t) size);
		GGANetStoreU32(&frame[5], off);
		GGANetStoreU32(&frame[9], hash);
		if (n) {
			memcpy(&frame[GGA_NET_BLOB_HEADER], (const uint8_t*) bytes + off, n);
		}
		if (!_outboxAppend(ep, 0x26, frame, (uint16_t) (GGA_NET_BLOB_HEADER + n))) {
			ok = false;
			break;
		}
		off += n;
	} while (off < size);
	free(frame);
	return ok;
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
	(void) fixed;
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
	if (id < 32) {
		ep->statTx[id]++;
	}
	/* TEMP DIAG (revert): see who sends what, with ms timestamps. */
	int64_t nowMs = 0;
	if (ep->link && ep->link->transport.nowMs) {
		nowMs = ep->link->transport.nowMs(ep->link->transport.context);
	}
	fprintf(stderr, "%lld NET tx id=%u len=%u\n", (long long) nowMs, id, (unsigned) payLen);
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

static void _defer(struct GBASIONetEndpoint* ep, uint8_t id, const uint8_t* pay, uint16_t len) {
	if (ep->nDeferred >= GGA_NET_DEFER_MAX || len > GGA_NET_DEFER_PAY) {
		fprintf(stderr, "NET defer DROP id=%u\n", id);
		return;
	}
	ep->deferred[ep->nDeferred].id = id;
	ep->deferred[ep->nDeferred].len = len;
	memcpy(ep->deferred[ep->nDeferred].pay, pay, len);
	++ep->nDeferred;
	fprintf(stderr, "NET deferred id=%u n=%zu\n", id, ep->nDeferred);
}

/* Route one frame into the lockstep driver. True = consumed (even if
 * malformed and dropped); false = coordinator busy, caller should defer. */
static bool _route(struct GBASIONetEndpoint* ep, uint8_t id, const uint8_t* pay, uint16_t len) {
	struct GBASIONetLink* link = ep->link;
	/* Single-peer: host hears slot 1, client hears slot 0. */
	int sender = ep->isHost ? 1 : 0;
	switch (id) {
	case 0x20: /* TRANSFER_START */
		if (len < 4) {
			return true;
		}
		return GBASIONetLinkOnWire(link, sender, 1, GGANetLoadI32(pay), 0, 0, 0);
	case 0x21: /* TRANSFER_DATA */
		if (len < 5) {
			return true;
		}
		return GBASIONetLinkOnTransferData(link, pay[0], GGANetLoadU32(&pay[1]));
	case 0x22: /* TRANSFER_RESULT */
		if (len < 17) {
			return true;
		}
		return GBASIONetLinkOnWire(link, sender, 6, 0, 0, pay, len);
	case 0x23: { /* EVENT */
		if (len < 6) {
			return true;
		}
		int ev = pay[0];
		int pl = pay[1];
		int32_t arg = GGANetLoadI32(&pay[2]);
		if (ev == 7) {
			/* ACK addressed by player in `pl`. */
			return GBASIONetLinkOnWire(link, pl, 7, pl, 0, 0, 0);
		}
		return GBASIONetLinkOnWire(link, sender, ev, arg, 0, 0, 0);
	}
	default:
		return true;
	}
}

static void _replayDeferred(struct GBASIONetEndpoint* ep) {
	size_t done = 0;
	while (done < ep->nDeferred) {
		if (ep->link->localSlot < 0) {
			break;
		}
		struct GGANetDeferred* d = &ep->deferred[done];
		if (!_route(ep, d->id, d->pay, d->len)) {
			break;
		}
		fprintf(stderr, "NET replayed id=%u\n", d->id);
		++done;
	}
	if (done > 0) {
		size_t rest = ep->nDeferred - done;
		memmove(&ep->deferred[0], &ep->deferred[done], rest * sizeof(ep->deferred[0]));
		ep->nDeferred = rest;
	}
}

static void _dispatch(struct GBASIONetEndpoint* ep, uint8_t id, const uint8_t* pay, uint16_t len) {
	struct GBASIONetLink* link = ep->link;
	/* TEMP DIAG (revert): ms timestamps. */
	int64_t nowMs = 0;
	if (link && link->transport.nowMs) {
		nowMs = link->transport.nowMs(link->transport.context);
	}
	fprintf(stderr, "%lld NET rx id=%u len=%u\n", (long long) nowMs, id, len);
	if (id < 32) {
		ep->statRx[id]++;
	}
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
			ep->peerHello = true;
			/* v2 startup: seed the RTC, then share the ROM and save
			 * so the client can boot the same session. */
			if (link->transport.getRtcSeed) {
				int64_t seed = 0;
				link->transport.getRtcSeed(link->transport.context, &seed);
				GGANetSendRtcSeed(ep->conn, seed);
			}
			if (link->transport.getBlob) {
				static const uint8_t kinds[2] = { GGA_NET_BLOB_ROM, GGA_NET_BLOB_SAVE };
				int ki;
				for (ki = 0; ki < 2; ++ki) {
					void* bytes = 0;
					size_t size = 0;
					if (link->transport.getBlob(link->transport.context, kinds[ki], &bytes, &size) && bytes) {
						_queueBlob(ep, kinds[ki], bytes, size);
						free(bytes);
					}
				}
			}
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
		if (len >= 1) {
			ep->rejectReason = pay[0];
		}
		if (ep->session) {
			GBASIONetSessionOnPeerGone(ep->session, GGA_NET_END_REJECT);
		}
		break;
	case 0x20: /* TRANSFER_START */
	case 0x21: /* TRANSFER_DATA */
	case 0x22: /* TRANSFER_RESULT */
	case 0x23: /* EVENT */
		if (link->localSlot < 0 || !_route(ep, id, pay, len)) {
			_defer(ep, id, pay, len);
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
	case 0x26: { /* BLOB chunk: reassemble, verify hash, hand to the app */
		if (len < GGA_NET_BLOB_HEADER) {
			break;
		}
		uint8_t kind = pay[0];
		uint32_t total = GGANetLoadU32(&pay[1]);
		uint32_t off = GGANetLoadU32(&pay[5]);
		uint32_t hash = GGANetLoadU32(&pay[9]);
		size_t n;
		if (total > 32 * 1024 * 1024) {
			break;
		}
		if (!ep->blobActive || ep->blobTotal != total || ep->blobKind != kind) {
			free(ep->blobReasm);
			ep->blobReasm = malloc(total ? total : 1);
			ep->blobActive = !!ep->blobReasm;
			ep->blobTotal = total;
			ep->blobKind = kind;
			ep->blobHash = hash;
			ep->blobGot = 0;
		}
		if (!ep->blobActive) {
			break;
		}
		n = len - GGA_NET_BLOB_HEADER;
		if (off > total || n > total - off) {
			break;
		}
		memcpy(ep->blobReasm + off, pay + GGA_NET_BLOB_HEADER, n);
		if ((size_t) off + n > ep->blobGot) {
			ep->blobGot = off + n;
		}
		if (ep->blobGot >= total) {
			if (GGANetHash32(ep->blobReasm, total) == hash && link->transport.loadBlob) {
				link->transport.loadBlob(link->transport.context, kind, ep->blobReasm, total);
			}
			free(ep->blobReasm);
			ep->blobReasm = 0;
			ep->blobActive = false;
			ep->blobTotal = ep->blobGot = 0;
		}
		break;
	}
	case 0x27: /* RTC seed */
		if (len >= 8 && link->transport.loadRtcSeed) {
			uint64_t ms = ((uint64_t) GGANetLoadU32(&pay[0]) << 32) | GGANetLoadU32(&pay[4]);
			link->transport.loadRtcSeed(link->transport.context, (int64_t) ms);
		}
		break;
	case 0x28: /* INPUT (guest -> host) */
		if (ep->session && len >= GGA_NET_INPUT_SIZE) {
			GBASIONetSessionOnInput(ep->session, GGANetLoadU32(pay), GGANetLoadU16(&pay[4]));
		}
		break;
	case 0x29: { /* TICK (host -> all): [u32 frame][u16 keys x n] */
		if (ep->session && len >= 6 && (len - 4) % 2 == 0 && (len - 4) / 2 <= GGA_NET_MAX_PLAYERS) {
			uint16_t keys[GGA_NET_MAX_PLAYERS] = { 0 };
			uint8_t n = (uint8_t) ((len - 4) / 2);
			uint8_t i;
			for (i = 0; i < n; ++i) {
				keys[i] = GGANetLoadU16(&pay[4 + 2 * i]);
			}
			GBASIONetSessionOnTick(ep->session, GGANetLoadU32(pay), keys, n);
		}
		break;
	}
	case 0x2A: { /* HASH (both): [u32 frame][u8 n][u32 hash x n] */
		uint8_t n;
		if (!ep->session || len < GGA_NET_HASH_HEADER) {
			break;
		}
		n = pay[4];
		if (!n || n > GGA_NET_MAX_PLAYERS || len != GGA_NET_HASH_HEADER + 4 * n) {
			break;
		}
		uint32_t hashes[GGA_NET_MAX_PLAYERS] = { 0 };
		uint8_t i;
		for (i = 0; i < n; ++i) {
			hashes[i] = GGANetLoadU32(&pay[GGA_NET_HASH_HEADER + 4 * i]);
		}
		GBASIONetSessionOnHash(ep->session, GGANetLoadU32(pay), hashes, n);
		break;
	}
	case 0x2B: /* PAUSE */
	case 0x2C: /* RESUME */
	case 0x2D: /* STOP */
		if (ep->session && len >= 1) {
			GBASIONetSessionOnControl(ep->session, id, pay[0]);
		}
		break;
	case 0x06:
	case 0x07:
		if (id == 0x07) {
			ep->peerBye = true;
			if (ep->session) {
				GBASIONetSessionOnPeerGone(ep->session, GGA_NET_END_PEER_BYE);
			}
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
			fprintf(stderr, "NET accepted peer\n");
			SocketSetBlocking(peer, false);
			SocketSetTCPPush(peer, 1);
			ep->conn = peer;
		} else if (!SocketWouldBlock()) {
			fprintf(stderr, "NET accept error %d\n", SocketError());
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
	if (ep->session) {
		GBASIONetSessionOnPeerGone(ep->session, GGA_NET_END_PEER_GONE);
	}
	_flushOutbox(ep);
polled:
	_replayDeferred(ep);
	GBASIONetLinkPoll(ep->link);
}
