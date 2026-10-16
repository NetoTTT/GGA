/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_NETDRIVER_H
#define GBA_SIO_NETDRIVER_H

/* Network bridge for the lockstep coordinator (GGA).
 *
 * Each device runs one coordinator: 1 REAL player (local core) plus one
 * PROXY player per remote peer (no thread; driven via AddProxy and
 * ProxyEnqueue). The host (player 0) also arbitrates: it collects slot
 * data and broadcasts the completed RESULT so every device finishes
 * transfers with identical bytes. Late/missing slots resolve to 0xFF,
 * mirroring the built-in behind-rule, decided once by the host.
 *
 * Transport is injected (see gga/net transport or any byte pipe):
 *   sendMsg(link, targetPlayer, type, arg, data, len)
 * Incoming wire bytes enter via GBASIONetLinkOnWire().
 */
#include <mgba-util/common.h>
#include <mgba/internal/gba/sio/lockstep.h>

CXX_GUARD_START

#define GGA_NET_MAX_PEERS 3
#define GGA_NET_STALL_BUDGET_MS 2000 /* TEMP experiment: is the peer just slow? */

enum GGANetEventKind {
	GGA_NET_EV_TRANSFER_START = 1,
	GGA_NET_EV_MODE_SET = 2,
	GGA_NET_EV_HARD_SYNC = 3,
	GGA_NET_EV_ATTACH = 4,
	GGA_NET_EV_DETACH = 5,
	GGA_NET_EV_RESULT = 6
};

struct GBASIONetLink;

/* Transport callbacks provided by the embedding application. */
struct GBASIONetTransport {
	void* context;
	void (*send)(void* context, int targetPlayer, int kind, int32_t a, int32_t b,
		const void* data, size_t len);
	/* Milliseconds since an arbitrary epoch, for stall budgets. */
	int64_t (*nowMs)(void* context);
	/* Full-state bytes for late join (frontend-provided, may be NULL). */
	bool (*saveState)(void* context, void** bytes, size_t* size);
	void (*loadState)(void* context, const void* bytes, size_t size);
};

struct GBASIONetProxyUser {
	struct mLockstepUser d;
	int wantedSlot;
};

struct GBASIONetLink {
	struct GBASIOLockstepCoordinator* coordinator;
	struct GBASIOLockstepDriver* realDriver;
	struct mLockstepUser chainUser;
	struct mLockstepUser* chainedUser;
	struct GBASIONetTransport transport;

	struct GBASIOLockstepDriver proxies[GGA_NET_MAX_PEERS];
	struct GBASIONetProxyUser proxyUsers[GGA_NET_MAX_PEERS];
	bool proxyActive[GGA_NET_MAX_PEERS];
	bool slotAcked[MAX_GBAS];

	int localSlot; /* -1 until assigned */
	int assignedSlot;
	bool isHost;
	int64_t transferStartMs;
	bool transferSeen;
	/* Remote contribution tracking: transferPending set when a
	 * TRANSFER_START is injected; ackSeen set when our thread sleeps
	 * afterwards (= data latched). Send once, then clear both. */
	bool transferPending;
	bool ackSeen;
	/* Barrier ack tracking (ATTACH/HARD_SYNC/MODE_SET): set at inject,
	 * sent once our thread acks locally. */
	bool barrierPending;
};

/* Transport kinds (netdriver -> transport). 1-6 mirror GGANetEventKind;
 * 0 carries a remote slot contribution: payload [slot u8][value u32 LE];
 * 7 is a barrier ack: a = acking slot. */
#define GGA_NETW_TRANSFER_DATA 0
#define GGA_NETW_ACK 7

void GBASIONetLinkInit(struct GBASIONetLink* link, struct GBASIOLockstepCoordinator* coordinator,
	struct GBASIOLockstepDriver* realDriver, struct mLockstepUser* chainedUser);
void GBASIONetLinkDeinit(struct GBASIONetLink* link);

/* Add/remove a remote peer (proxy slot). Any slot in [0, 3]; the caller
 * must not add a proxy for its own slot. */
bool GBASIONetLinkAddPeer(struct GBASIONetLink* link, int wantedSlot);
void GBASIONetLinkRemovePeer(struct GBASIONetLink* link, int slot);

/* Pump: forward queued proxy events to the wire; enforce stall budget.
 * Call frequently from the network thread. Uses blocking locks:
 * critical sections are short and never nest. */
void GBASIONetLinkPoll(struct GBASIONetLink* link);

/* Incoming wire message (already framed by net.c or equivalent).
 * Returns false only when the link is not attached yet: defer the
 * frame and retry after attach (ordering is the caller's
 * responsibility). */
bool GBASIONetLinkOnWire(struct GBASIONetLink* link, int senderSlot,
	int kind, int32_t a, int32_t b, const void* data, size_t len);

/* Session start: align the shared clock to local time (see .c). */
void GBASIONetLinkResetClock(struct GBASIONetLink* link);

/* Session start: drop a stale asleep flag (see .c). Call paused. */
void GBASIONetLinkClearStaleSleep(struct GBASIONetLink* link);

/* Remote slot contribution (host side): fill + ack the proxy.
 * Same defer-on-unattached contract as OnWire. */
bool GBASIONetLinkOnTransferData(struct GBASIONetLink* link, int slot, uint32_t value);

CXX_GUARD_END

#endif
