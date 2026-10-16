/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_NETEP_H
#define GBA_SIO_NETEP_H

/* TCP endpoint for GGA netplay (see docs/NETPLAY.md).
 *
 * Pump-driven (no threads inside): the frontend calls
 * GBASIONetEndpointPoll() frequently. All sockets are non-blocking;
 * outgoing bytes wait in an outbox. One endpoint = one peer; a host
 * runs one endpoint per client.
 *
 * Handshake: client sends HELLO on connect; host validates
 * (proto/core/rom) and replies WELCOME (assigns slot) or REJECT.
 * STATE blobs are chunked to fit the 64 KiB framing.
 */
#include <mgba-util/common.h>
#include <mgba-util/socket.h>
#include <mgba/internal/gba/sio/net.h>
#include <mgba/internal/gba/sio/netdriver.h>

CXX_GUARD_START

#define GGA_NET_CHUNK 60000
#define GGA_NET_DEFER_MAX 32
#define GGA_NET_DEFER_PAY 32

struct GBASIONetLink;

struct GGANetDeferred {
	uint8_t id;
	uint16_t len;
	uint8_t pay[GGA_NET_DEFER_PAY];
};

struct GBASIONetEndpoint {
	struct GBASIONetLink* link;
	Socket listenSock;
	Socket conn;
	bool isHost;
	bool welcomed; /* client side */
	bool peerBye;
	bool peerHello; /* host side: a client knocked (start session) */
	uint8_t rejectReason; /* valid when peerBye came from REJECT */

	uint8_t romHash[32];
	char name[64];

	uint8_t* outbox;
	size_t outboxLen;
	size_t outboxCap;

	uint8_t* inReasm;
	size_t inReasmTotal;
	size_t inReasmGot;
	uint8_t inbuf[GGA_NET_MAX_PAYLOAD];
	uint8_t hdrAcc[3];
	size_t hdrGot;
	uint8_t inId;
	uint16_t inPayLen;
	size_t inPayGot;
	long statTx[32];
	long statRx[32];

	/* Frames that arrived before the local driver attached (or while
	 * the coordinator mutex was busy): replayed in order once ready. */
	struct GGANetDeferred deferred[GGA_NET_DEFER_MAX];
	size_t nDeferred;
};

void GBASIONetEndpointInit(struct GBASIONetEndpoint* ep, struct GBASIONetLink* link,
	const struct GBASIONetTransport* frontend);
void GBASIONetEndpointSetIdentity(struct GBASIONetEndpoint* ep, const uint8_t romHash[32], const char* name);
void GBASIONetEndpointClose(struct GBASIONetEndpoint* ep);

/* Host: bind+listen (non-blocking accept happens in Poll). Client:
 * connect (blocking, session start only). Returns false on setup error. */
bool GBASIONetEndpointListen(struct GBASIONetEndpoint* ep, int port);
bool GBASIONetEndpointConnect(struct GBASIONetEndpoint* ep, const char* host, int port);

/* Pump once: accept (host), recv+dispatch, flush outbox, Poll(link). */
void GBASIONetEndpointPoll(struct GBASIONetEndpoint* ep);

CXX_GUARD_END

#endif
