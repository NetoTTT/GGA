/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/netsession.h>

#include <mgba/internal/gba/sio/net.h>
#include <mgba/internal/gba/sio/netep.h>

#include <string.h>

static int64_t _now(struct GBASIONetSession* sess) {
	return sess->nowMs ? sess->nowMs(sess->context) : 0;
}

bool GBASIONetSessionEnded(const struct GBASIONetSession* sess) {
	return sess->stopped || sess->failed || sess->desync;
}

uint8_t GBASIONetSessionEndReason(const struct GBASIONetSession* sess) {
	return sess->endReason;
}

/* First reason wins: a desync that trips while the peer was already gone
 * should not be rewritten by the later notice. */
static void _end(struct GBASIONetSession* sess, uint8_t reason) {
	if (!sess->endReason) {
		sess->endReason = reason;
	}
	sess->stopped = true;
}

static void _fail(struct GBASIONetSession* sess) {
	if (!sess->endReason) {
		sess->endReason = GGA_NET_END_PROTOCOL;
	}
	sess->failed = true;
}

static void _sendControl(struct GBASIONetSession* sess, uint8_t id, uint8_t reason) {
	if (sess->peer) {
		GBASIONetEndpointSend(sess->peer, id, &reason, 1);
	}
}

void GBASIONetSessionInit(struct GBASIONetSession* sess, int role, int localPlayer, int nPlayers,
	int64_t (*nowMs)(void* context), void* context) {
	memset(sess, 0, sizeof(*sess));
	sess->role = role;
	sess->localPlayer = localPlayer;
	sess->nPlayers = nPlayers;
	sess->nowMs = nowMs;
	sess->context = context;
	sess->inputDeadlineMs = GGA_NET_INPUT_DEADLINE_MS;
	sess->waitSinceMs = -1;
	/* 4-player link is Phase 7 of the architecture doc. */
	if (nPlayers != 2 || localPlayer < 0 || localPlayer > 1) {
		_fail(sess);
	}
}

void GBASIONetSessionSetPeer(struct GBASIONetSession* sess, struct GBASIONetEndpoint* peer) {
	sess->peer = peer;
	if (peer) {
		peer->session = sess;
	}
}

void GBASIONetSessionSetLocalInput(struct GBASIONetSession* sess, uint16_t keys) {
	sess->localKeys = keys;
}

uint32_t GBASIONetSessionFrame(const struct GBASIONetSession* sess) {
	return sess->frame;
}

void GBASIONetSessionStart(struct GBASIONetSession* sess) {
	uint8_t pay[GGA_NET_INPUT_SIZE];
	uint32_t f;
	if (sess->role != GGA_NET_SESSION_GUEST || !sess->peer || sess->frame != 0) {
		return;
	}
	/* Announce the frames the host will run before our first answer. */
	for (f = 0; f < GGA_NET_INPUT_DELAY; ++f) {
		GGANetStoreU32(pay, f);
		GGANetStoreU16(&pay[4], sess->localKeys);
		GBASIONetEndpointSend(sess->peer, GGA_NET_INPUT, pay, sizeof(pay));
	}
}

void GBASIONetSessionSetPaused(struct GBASIONetSession* sess, bool paused) {
	if (sess->paused == paused) {
		return;
	}
	sess->paused = paused;
	sess->waitSinceMs = -1;
	if (sess->role == GGA_NET_SESSION_HOST) {
		_sendControl(sess, paused ? GGA_NET_PAUSE : GGA_NET_RESUME, 0);
	}
}

void GBASIONetSessionStop(struct GBASIONetSession* sess, uint8_t reason) {
	if (sess->role == GGA_NET_SESSION_HOST) {
		_sendControl(sess, GGA_NET_STOP, reason);
	}
	_end(sess, GGA_NET_END_LOCAL);
}

/* Host: resolve frame `sess->frame` and broadcast it as the authoritative
 * tick. Its own keys are sampled now; the guest's come from its INPUT for
 * this exact frame. Missing input is only filled with the idle mask once the
 * deadline passes, and never once the guest has fallen further behind than
 * the ahead window -- that is the backpressure stop. */
static bool _runHost(struct GBASIONetSession* sess, uint16_t* keys) {
	uint32_t f = sess->frame;
	uint32_t slot = f % GGA_NET_TICK_WINDOW;
	uint32_t guestRun = sess->guestHigh > GGA_NET_INPUT_DELAY
		? sess->guestHigh - GGA_NET_INPUT_DELAY : 0;
	uint8_t pay[4 + 2 * GGA_NET_MAX_PLAYERS];
	uint16_t guestKeys;

	if (sess->guestValid[slot] && sess->guestFrame[slot] == f) {
		guestKeys = sess->guestKeys[slot];
		sess->guestValid[slot] = false;
	} else if (f > guestRun + GGA_NET_TICK_AHEAD) {
		return false;
	} else {
		int64_t now = _now(sess);
		if (sess->waitSinceMs < 0) {
			sess->waitSinceMs = now;
		}
		if (now - sess->waitSinceMs < sess->inputDeadlineMs) {
			return false;
		}
		guestKeys = 0;
		sess->lagging = true;
	}
	sess->waitSinceMs = -1;

	memset(keys, 0, sizeof(uint16_t) * GGA_NET_MAX_PLAYERS);
	keys[sess->localPlayer] = sess->localKeys;
	keys[1 - sess->localPlayer] = guestKeys;

	GGANetStoreU32(pay, f);
	GGANetStoreU16(&pay[4], keys[0]);
	GGANetStoreU16(&pay[6], keys[1]);
	if (sess->peer) {
		GBASIONetEndpointSend(sess->peer, GGA_NET_TICK, pay, 8);
	}
	++sess->frame;
	return true;
}

/* Guest: run the frame the host ticked, then announce our keys for a frame
 * DELAY ahead. That announcement doubles as the ack of everything below it. */
static bool _runGuest(struct GBASIONetSession* sess, uint16_t* keys) {
	uint32_t f = sess->frame;
	uint32_t slot = f % GGA_NET_TICK_WINDOW;
	uint8_t pay[GGA_NET_INPUT_SIZE];
	uint8_t n;

	if (!sess->tickValid[slot] || sess->tickFrame[slot] != f) {
		return false;
	}
	n = sess->tickN[slot];
	if (n > GGA_NET_MAX_PLAYERS) {
		n = GGA_NET_MAX_PLAYERS;
	}
	memset(keys, 0, sizeof(uint16_t) * GGA_NET_MAX_PLAYERS);
	memcpy(keys, sess->tickKeys[slot], sizeof(uint16_t) * n);
	sess->tickValid[slot] = false;
	++sess->frame;

	GGANetStoreU32(pay, f + GGA_NET_INPUT_DELAY);
	GGANetStoreU16(&pay[4], sess->localKeys);
	if (sess->peer) {
		GBASIONetEndpointSend(sess->peer, GGA_NET_INPUT, pay, sizeof(pay));
	}
	return true;
}

bool GBASIONetSessionNextFrame(struct GBASIONetSession* sess, uint16_t* keys) {
	if (sess->failed || sess->stopped || sess->paused || sess->desync) {
		return false;
	}
	if (sess->role == GGA_NET_SESSION_HOST) {
		return _runHost(sess, keys);
	}
	return _runGuest(sess, keys);
}

/* Record one side's sampled-frame hashes and compare as soon as both sides
 * are in, so a divergence is caught whichever copy arrives last. */
static void _hashNote(struct GBASIONetSession* sess, uint32_t frame, bool mine,
	const uint32_t* hashes, uint8_t nPlayers) {
	uint32_t slot = (frame / GGA_NET_HASH_INTERVAL) % GGA_NET_HASH_WINDOW;
	if (sess->hashFrame[slot] != frame) {
		sess->hashFrame[slot] = frame;
		sess->hashMineValid[slot] = false;
		sess->hashTheirsValid[slot] = false;
	}
	if (mine) {
		memcpy(sess->hashMine[slot], hashes, sizeof(uint32_t) * nPlayers);
		sess->hashMineValid[slot] = true;
	} else {
		memcpy(sess->hashTheirs[slot], hashes, sizeof(uint32_t) * nPlayers);
		sess->hashTheirsValid[slot] = true;
	}
	if (sess->hashMineValid[slot] && sess->hashTheirsValid[slot] &&
		memcmp(sess->hashMine[slot], sess->hashTheirs[slot], sizeof(uint32_t) * nPlayers) != 0) {
		if (!sess->endReason) {
			sess->endReason = GGA_NET_END_DESYNC;
		}
		sess->desync = true;
	}
}

bool GBASIONetSessionSampling(const struct GBASIONetSession* sess) {
	return sess->frame != 0 && sess->frame % GGA_NET_HASH_INTERVAL == 0;
}

void GBASIONetSessionFrameDone(struct GBASIONetSession* sess, const uint32_t* hashes,
	uint8_t nPlayers) {
	uint8_t pay[GGA_NET_HASH_HEADER + 4 * GGA_NET_MAX_PLAYERS];
	uint32_t f;
	uint8_t i;

	if (sess->desync || sess->frame == 0 || nPlayers != sess->nPlayers) {
		return;
	}
	f = sess->frame - 1; /* the frame that just ran */
	if (sess->frame % GGA_NET_HASH_INTERVAL != 0) {
		return;
	}
	_hashNote(sess, f, true, hashes, nPlayers);
	if (!sess->peer) {
		return;
	}
	GGANetStoreU32(&pay[0], f);
	pay[4] = nPlayers;
	for (i = 0; i < nPlayers; ++i) {
		GGANetStoreU32(&pay[GGA_NET_HASH_HEADER + 4 * i], hashes[i]);
	}
	GBASIONetEndpointSend(sess->peer, GGA_NET_HASH, pay,
		(uint16_t) (GGA_NET_HASH_HEADER + 4 * nPlayers));
}

void GBASIONetSessionOnHash(struct GBASIONetSession* sess, uint32_t frame,
	const uint32_t* hashes, uint8_t nPlayers) {
	if (sess->desync || !nPlayers || nPlayers != sess->nPlayers) {
		return;
	}
	_hashNote(sess, frame, false, hashes, nPlayers);
}

void GBASIONetSessionOnInput(struct GBASIONetSession* sess, uint32_t frame, uint16_t keys) {
	uint32_t slot;
	if (sess->role != GGA_NET_SESSION_HOST) {
		return;
	}
	if (frame < sess->frame) {
		return; /* already run (a late announcement, not an error) */
	}
	if (frame >= sess->frame + GGA_NET_TICK_WINDOW) {
		_fail(sess);
		return;
	}
	slot = frame % GGA_NET_TICK_WINDOW;
	sess->guestFrame[slot] = frame;
	sess->guestKeys[slot] = keys;
	sess->guestValid[slot] = true;
	if (frame > sess->guestHigh) {
		sess->guestHigh = frame;
	}
}

void GBASIONetSessionOnTick(struct GBASIONetSession* sess, uint32_t frame,
	const uint16_t* keys, uint8_t nPlayers) {
	uint32_t slot;
	if (sess->role != GGA_NET_SESSION_GUEST) {
		return;
	}
	if (frame < sess->frame) {
		return; /* duplicate of a frame we already ran */
	}
	if (frame >= sess->frame + GGA_NET_TICK_WINDOW) {
		_fail(sess); /* a gap we cannot fill */
		return;
	}
	if (!nPlayers || nPlayers > GGA_NET_MAX_PLAYERS) {
		_fail(sess);
		return;
	}
	slot = frame % GGA_NET_TICK_WINDOW;
	sess->tickFrame[slot] = frame;
	memcpy(sess->tickKeys[slot], keys, sizeof(uint16_t) * nPlayers);
	sess->tickN[slot] = nPlayers;
	sess->tickValid[slot] = true;
}

void GBASIONetSessionOnControl(struct GBASIONetSession* sess, uint8_t id, uint8_t reason) {
	(void) reason;
	switch (id) {
	case GGA_NET_PAUSE:
		sess->paused = true;
		break;
	case GGA_NET_RESUME:
		sess->paused = false;
		sess->waitSinceMs = -1;
		break;
	case GGA_NET_STOP:
		_end(sess, GGA_NET_END_PEER_STOP);
		break;
	default:
		break;
	}
}

void GBASIONetSessionOnPeerGone(struct GBASIONetSession* sess, uint8_t endReason) {
	_end(sess, endReason);
}
