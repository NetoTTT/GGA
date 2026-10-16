/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/netdriver.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio.h>
#include <mgba-util/table.h>

#include <string.h>

#define PROXY_TARGET(P) (1u << (P))

static void _proxySleep(struct mLockstepUser* user) {
	UNUSED(user);
}

static void _proxyWake(struct mLockstepUser* user) {
	UNUSED(user);
}

static int _proxyRequestedId(struct mLockstepUser* user) {
	struct GBASIONetProxyUser* proxy = (struct GBASIONetProxyUser*) user;
	fprintf(stderr, "DBG reqId proxy -> %d\n", proxy->wantedSlot);
	return proxy->wantedSlot;
}

static void _chainSleep(struct mLockstepUser* user) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) ((char*) user - offsetof(struct GBASIONetLink, chainUser));
	link->ackSeen = true;
	if (link->chainedUser && link->chainedUser->sleep) {
		link->chainedUser->sleep(link->chainedUser);
	}
}

static void _chainWake(struct mLockstepUser* user) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) ((char*) user - offsetof(struct GBASIONetLink, chainUser));
	if (link->chainedUser && link->chainedUser->wake) {
		link->chainedUser->wake(link->chainedUser);
	}
}

static int _chainRequestedId(struct mLockstepUser* user) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) ((char*) user - offsetof(struct GBASIONetLink, chainUser));
	fprintf(stderr, "DBG reqId chain -> %d\n", link->assignedSlot);
	return link->assignedSlot;
}

static void _chainPlayerIdChanged(struct mLockstepUser* user, int id) {
	struct GBASIONetLink* link = (struct GBASIONetLink*) ((char*) user - offsetof(struct GBASIONetLink, chainUser));
	link->localSlot = id;
	link->isHost = (id == 0);
	fprintf(stderr, "DBG idChanged -> %d\n", id);
	if (link->chainedUser && link->chainedUser->playerIdChanged) {
		link->chainedUser->playerIdChanged(link->chainedUser, id);
	}
}

void GBASIONetLinkInit(struct GBASIONetLink* link, struct GBASIOLockstepCoordinator* coordinator,
	struct GBASIOLockstepDriver* realDriver, struct mLockstepUser* chainedUser) {
	memset(link, 0, sizeof(*link));
	link->coordinator = coordinator;
	link->realDriver = realDriver;
	link->chainedUser = chainedUser;
	link->chainUser.sleep = _chainSleep;
	link->chainUser.wake = _chainWake;
	link->chainUser.requestedId = _chainRequestedId;
	link->chainUser.playerIdChanged = _chainPlayerIdChanged;
	link->localSlot = -1;
	link->assignedSlot = -1;
	link->isHost = false;
	realDriver->user = &link->chainUser;
}

void GBASIONetLinkDeinit(struct GBASIONetLink* link) {
	int i;
	for (i = 0; i < GGA_NET_MAX_PEERS; ++i) {
		if (link->proxyActive[i]) {
			GBASIOLockstepCoordinatorDetach(link->coordinator, &link->proxies[i]);
			link->proxyActive[i] = false;
		}
	}
	if (link->realDriver) {
		link->realDriver->user = link->chainedUser;
	}
}

bool GBASIONetLinkAddPeer(struct GBASIONetLink* link, int wantedSlot) {
	if (wantedSlot < 0 || wantedSlot >= MAX_GBAS) {
		return false;
	}
	int i;
	for (i = 0; i < GGA_NET_MAX_PEERS; ++i) {
		if (!link->proxyActive[i]) {
			break;
		}
	}
	if (i == GGA_NET_MAX_PEERS) {
		return false;
	}
	struct GBASIOLockstepDriver* proxy = &link->proxies[i];
	GBASIOLockstepDriverCreate(proxy, &link->proxyUsers[i].d);
	link->proxyUsers[i].wantedSlot = wantedSlot;
	link->proxyUsers[i].d.sleep = _proxySleep;
	link->proxyUsers[i].d.wake = _proxyWake;
	link->proxyUsers[i].d.requestedId = _proxyRequestedId;
	link->proxyUsers[i].d.playerIdChanged = 0;
	/* Borrow local timing; proxies never run threads. */
	proxy->d.p = link->realDriver->d.p;
	GBASIOLockstepCoordinatorAddProxy(link->coordinator, proxy);
	link->proxyActive[i] = true;
	return true;
}

void GBASIONetLinkRemovePeer(struct GBASIONetLink* link, int slot) {
	int i;
	for (i = 0; i < GGA_NET_MAX_PEERS; ++i) {
		if (!link->proxyActive[i]) {
			continue;
		}
		struct GBASIOLockstepPlayer* player = TableLookup(
			&link->coordinator->players, link->proxies[i].lockstepId);
		if (player && player->playerId == slot) {
			GBASIOLockstepCoordinatorDetach(link->coordinator, &link->proxies[i]);
			link->proxyActive[i] = false;
			return;
		}
	}
}

static struct GBASIOLockstepPlayer* _findProxy(struct GBASIONetLink* link, int slot) {
	int i;
	for (i = 0; i < GGA_NET_MAX_PEERS; ++i) {
		if (!link->proxyActive[i]) {
			continue;
		}
		struct GBASIOLockstepPlayer* player = TableLookup(
			&link->coordinator->players, link->proxies[i].lockstepId);
		if (player && player->playerId == slot) {
			return player;
		}
	}
	return 0;
}

static int32_t _localNow(struct GBASIONetLink* link) {
	return mTimingCurrentTime(&link->realDriver->d.p->p->timing);
}

static void _writeSlot(struct GBASIONetLink* link, int slot, uint32_t value) {
	if (link->coordinator->transferMode == GBA_SIO_MULTI) {
		link->coordinator->multiData[slot] = (uint16_t) value;
	} else {
		link->coordinator->normalData[slot] = value;
	}
}

static void _tapProxies(struct GBASIONetLink* link) {
	int i;
	for (i = 0; i < GGA_NET_MAX_PEERS; ++i) {
		if (!link->proxyActive[i]) {
			continue;
		}
		struct GBASIOLockstepPlayer* player = TableLookup(
			&link->coordinator->players, link->proxies[i].lockstepId);
		if (!player) {
			continue;
		}
		int slot = player->playerId;
		struct GBASIOLockstepEvent* ev = player->queue;
		while (ev) {
			int kind = 0;
			int32_t arg = 0;
			switch (ev->type) {
			case SIO_EV_TRANSFER_START:
				kind = GGA_NET_EV_TRANSFER_START;
				arg = ev->finishCycle;
				if (!link->transferSeen) {
					link->transferSeen = true;
					if (link->transport.nowMs) {
						link->transferStartMs = link->transport.nowMs(link->transport.context);
					}
					memset(link->slotAcked, 0, sizeof(link->slotAcked));
					if (link->localSlot >= 0) {
						link->slotAcked[link->localSlot] = true;
					}
				}
				break;
			case SIO_EV_MODE_SET:
				kind = GGA_NET_EV_MODE_SET;
				arg = ev->mode;
				break;
			case SIO_EV_HARD_SYNC:
				kind = GGA_NET_EV_HARD_SYNC;
				break;
			case SIO_EV_ATTACH:
				kind = GGA_NET_EV_ATTACH;
				break;
			case SIO_EV_DETACH:
				kind = GGA_NET_EV_DETACH;
				break;
			}
			if (kind && link->transport.send) {
				link->transport.send(link->transport.context, slot, kind, arg, 0, 0, 0);
			}
			player->queue = ev->next;
			ev->next = player->freeList;
			player->freeList = ev;
			ev = player->queue;
		}
	}
}

static void _broadcastResult(struct GBASIONetLink* link) {
	uint8_t payload[17];
	payload[0] = (uint8_t) link->coordinator->transferMode;
	int i;
	for (i = 0; i < 4; ++i) {
		uint32_t v = (link->coordinator->transferMode == GBA_SIO_MULTI)
			? link->coordinator->multiData[i]
			: link->coordinator->normalData[i];
		payload[1 + i * 4] = v & 0xFF;
		payload[2 + i * 4] = (v >> 8) & 0xFF;
		payload[3 + i * 4] = (v >> 16) & 0xFF;
		payload[4 + i * 4] = (v >> 24) & 0xFF;
	}
	if (link->transport.send) {
		link->transport.send(link->transport.context, -1, GGA_NET_EV_RESULT, 0, 0, payload, sizeof(payload));
	}
}

static void _enforceStall(struct GBASIONetLink* link) {
	if (!link->isHost || !link->transferSeen || !link->transport.nowMs) {
		return;
	}
	if (!link->coordinator->transferActive) {
		return;
	}
	int64_t now = link->transport.nowMs(link->transport.context);
	if (now - link->transferStartMs < GGA_NET_STALL_BUDGET_MS) {
		return;
	}
	int i;
	for (i = 0; i < link->coordinator->nAttached; ++i) {
		if (link->slotAcked[i]) {
			continue;
		}
		_writeSlot(link, i, 0xFFFFFFFFu);
		link->slotAcked[i] = true;
		struct GBASIOLockstepPlayer* proxy = _findProxy(link, i);
		if (proxy) {
			GBASIOLockstepCoordinatorAckPlayer(link->coordinator, proxy);
		}
	}
}

/* Pump paths use blocking locks: critical sections are short and never
 * nest, and opportunistic try-locks were observed to starve here. */
void GBASIONetLinkPoll(struct GBASIONetLink* link) {
	MutexLock(&link->coordinator->mutex);
	_tapProxies(link);
	if (link->transferSeen && !link->coordinator->transferActive) {
		/* Transfer just completed: everyone (including us) agreed. */
		if (link->isHost) {
			_broadcastResult(link);
		}
		link->transferSeen = false;
	}
	if (!link->isHost && link->transferPending && link->ackSeen) {
		/* Our thread latched its slot: ship the contribution. */
		uint32_t v = (link->coordinator->transferMode == GBA_SIO_MULTI)
			? link->coordinator->multiData[link->localSlot]
			: link->coordinator->normalData[link->localSlot];
		uint8_t pay[5];
		pay[0] = (uint8_t) link->localSlot;
		pay[1] = v & 0xFF;
		pay[2] = (v >> 8) & 0xFF;
		pay[3] = (v >> 16) & 0xFF;
		pay[4] = (v >> 24) & 0xFF;
		if (link->transport.send) {
			link->transport.send(link->transport.context, 0,
				GGA_NETW_TRANSFER_DATA, 0, 0, pay, sizeof(pay));
		}
		link->transferPending = false;
		link->ackSeen = false;
	}
	if (!link->isHost && link->barrierPending && link->ackSeen) {
		if (link->transport.send) {
			link->transport.send(link->transport.context, 0,
				GGA_NETW_ACK, link->localSlot, 0, 0, 0);
		}
		link->barrierPending = false;
		link->ackSeen = false;
	}
	_enforceStall(link);
	MutexUnlock(&link->coordinator->mutex);
}

bool GBASIONetLinkOnWire(struct GBASIONetLink* link, int senderSlot,
	int kind, int32_t a, int32_t b, const void* data, size_t len) {
	UNUSED(b);
	UNUSED(len);
	if (link->localSlot < 0) {
		return false; /* Not attached yet: caller defers and retries. */
	}
	MutexLock(&link->coordinator->mutex);
	int32_t now = _localNow(link);
	switch (kind) {
	case GGA_NET_EV_TRANSFER_START: {
		/* Sender's finishCycle (a) is foreign-domain; recompute locally. */
		struct GBASIO* sio = link->realDriver->d.p;
		int32_t finish = now + GBASIOTransferCycles(link->coordinator->transferMode,
			sio->siocnt, link->coordinator->nAttached);
		GBASIOLockstepProxyEnqueue(link->coordinator, link->localSlot, senderSlot,
			SIO_EV_TRANSFER_START, now, finish);
		link->transferPending = true;
		link->ackSeen = false;
		break;
	}
	case GGA_NET_EV_MODE_SET:
		GBASIOLockstepProxyEnqueue(link->coordinator, link->localSlot, senderSlot,
			SIO_EV_MODE_SET, now, a);
		link->barrierPending = true;
		link->ackSeen = false;
		break;
	case GGA_NET_EV_HARD_SYNC:
		GBASIOLockstepProxyEnqueue(link->coordinator, link->localSlot, senderSlot,
			SIO_EV_HARD_SYNC, now, 0);
		link->barrierPending = true;
		link->ackSeen = false;
		break;
	case GGA_NET_EV_ATTACH:
		GBASIOLockstepProxyEnqueue(link->coordinator, link->localSlot, senderSlot,
			SIO_EV_ATTACH, now, 0);
		/* Attach is a barrier too: the peer must observe us before
		 * either side proceeds, otherwise both park forever. Unlike
		 * data barriers, attach carries no payload, so ack it
		 * immediately instead of waiting for our thread to latch:
		 * a freely-running peer would otherwise never ack and the
		 * other side would starve. */
		link->barrierPending = true;
		link->ackSeen = false;
		if (link->transport.send) {
			link->transport.send(link->transport.context, 0,
				GGA_NETW_ACK, link->localSlot, 0, 0, 0);
		}
		break;
	case GGA_NET_EV_DETACH:
		GBASIOLockstepProxyEnqueue(link->coordinator, link->localSlot, senderSlot,
			SIO_EV_DETACH, now, 0);
		if (kind == GGA_NET_EV_DETACH) {
			GBASIONetLinkRemovePeer(link, senderSlot);
		}
		break;
	case GGA_NET_EV_RESULT: {
		/* Host verdict: apply identical bytes, then wake our thread. */
		const uint8_t* p = data;
		int mode = p ? p[0] : (int) link->coordinator->transferMode;
		(void) mode;
		if (p) {
			int i;
			for (i = 0; i < 4; ++i) {
				uint32_t v = (uint32_t) p[1 + i * 4]
					| ((uint32_t) p[2 + i * 4] << 8)
					| ((uint32_t) p[3 + i * 4] << 16)
					| ((uint32_t) p[4 + i * 4] << 24);
				if (link->coordinator->transferMode == GBA_SIO_MULTI) {
					link->coordinator->multiData[i] = (uint16_t) v;
				} else {
					link->coordinator->normalData[i] = v;
				}
			}
		}
		if (link->chainedUser && link->chainedUser->wake) {
			link->chainedUser->wake(link->chainedUser);
		}
		break;
	}
	default:
		break;
	}
	if (!link->isHost && kind >= GGA_NET_EV_TRANSFER_START && kind <= GGA_NET_EV_DETACH) {
		/* Wake our thread so it processes the injected event. Without
		 * this, an idled secondary never runs to ack. The host primary
		 * must only leave a coordinator barrier when its ACK arrives. */
		if (link->chainedUser && link->chainedUser->wake) {
			link->chainedUser->wake(link->chainedUser);
		}
	}
	if (kind == GGA_NETW_TRANSFER_DATA && data && len >= 5) {
		const uint8_t* p = data;
		uint32_t v = (uint32_t) p[1] | ((uint32_t) p[2] << 8)
			| ((uint32_t) p[3] << 16) | ((uint32_t) p[4] << 24);
		GBASIONetLinkOnTransferData(link, p[0], v);
	} else if (kind == GGA_NETW_ACK) {
		struct GBASIOLockstepPlayer* proxy = _findProxy(link, a);
		if (proxy) {
			GBASIOLockstepCoordinatorAckPlayer(link->coordinator, proxy);
		}
	}
	MutexUnlock(&link->coordinator->mutex);
	return true;
}

/* TRANSFER_DATA (remote slot contribution, host side). Kind namespace
 * keeps wire ids stable with net.h framing. */
bool GBASIONetLinkOnTransferData(struct GBASIONetLink* link, int slot, uint32_t value) {
	MutexLock(&link->coordinator->mutex);
	_writeSlot(link, slot, value);
	if (slot >= 0 && slot < MAX_GBAS) {
		link->slotAcked[slot] = true;
	}
	struct GBASIOLockstepPlayer* proxy = _findProxy(link, slot);
	if (proxy) {
		GBASIOLockstepCoordinatorAckPlayer(link->coordinator, proxy);
	}
	MutexUnlock(&link->coordinator->mutex);
	return true;
}
