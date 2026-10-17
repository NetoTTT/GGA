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
struct GBASIONetSession;

struct GGANetDeferred {
	uint8_t id;
	uint16_t len;
	uint8_t pay[GGA_NET_DEFER_PAY];
};

struct GBASIONetEndpoint {
	struct GBASIONetLink* link;
	/* v2 input netplay: when set, INPUT/TICK/control frames are handed to
	 * this session instead of the lockstep link. */
	struct GBASIONetSession* session;
	Socket listenSock;
	Socket conn;
	bool isHost;
	bool welcomed; /* client side */
	bool peerBye;
	bool peerHello; /* host side: a client knocked (start session) */
	uint8_t rejectReason; /* valid when peerBye came from REJECT */

	uint8_t romHash[32];
	char name[64];

	/* Frontend-owned. Transport callbacks are handed the endpoint (see
	 * GBASIONetEndpointInit), so this is how they get back to the app. */
	void* userdata;

	uint8_t* outbox;
	size_t outboxLen;
	size_t outboxCap;

	uint8_t* inReasm;
	size_t inReasmTotal;
	size_t inReasmGot;
	/* Startup blob reassembly (GGA_NET_BLOB). Kept separate from
	 * inReasm so a late-join STATE transfer cannot interleave. */
	uint8_t* blobReasm;
	bool blobActive;
	uint8_t blobKind;
	uint32_t blobTotal;
	uint32_t blobHash;
	size_t blobGot;
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

/* Queue one frame for the peer without touching the sockets (safe to call
 * between polls, e.g. from the emulation loop). Drained by Poll. */
bool GBASIONetEndpointSend(struct GBASIONetEndpoint* ep, uint8_t id, const void* payload, uint16_t len);

CXX_GUARD_END

#endif
