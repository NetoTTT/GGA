/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_NETSESSION_H
#define GBA_SIO_NETSESSION_H

/* v2 input netplay: host-authoritative frame tick
 * (docs/NETPLAY-ARCHITECTURE.md, "Tick protocol").
 *
 * Every device runs all linked cores; the network carries input only, so
 * this layer never touches the emulation itself. The host owns the frame
 * counter and resolves one key mask per player per frame, sends TICK(F),
 * and the guest answers by announcing its own keys for a later frame.
 *
 * Guest input is delayed by GGA_NET_INPUT_DELAY frames: a guest that runs
 * frame F announces its keys for frame F + DELAY. That delay is the
 * pipeline that lets the host run at full speed instead of waiting a round
 * trip per frame, and it is why the guest pre-announces frames
 * 0..DELAY-1 at session start.
 *
 * Pump-driven like netep.c: no threads. Poll the endpoint often and call
 * NextFrame() from the emulation loop; step exactly one frame per true
 * result. The session only writes to the endpoint's outbox, which Poll
 * drains, so NextFrame() is safe to call between polls.
 *
 * This first cut is 2 players: one local player plus one remote.
 */
#include <mgba-util/common.h>
#include <mgba/internal/gba/sio/net.h>

CXX_GUARD_START

struct GBASIONetEndpoint;

/* Frames the guest's input is announced ahead of its own run position. */
#define GGA_NET_INPUT_DELAY 4
/* How far the host may run ahead of the guest's last run frame. */
#define GGA_NET_TICK_AHEAD 8
/* Tick ring size: enough to hold the ahead window plus the input delay. */
#define GGA_NET_TICK_WINDOW (GGA_NET_INPUT_DELAY + GGA_NET_TICK_AHEAD + 4)
/* How long the host waits for a frame's guest input before using the idle
 * mask. The guest keeps running while it waits, so this only spends slack. */
#define GGA_NET_INPUT_DEADLINE_MS 50
/* Hash every this many frames, comparing each player's state separately so a
 * divergence points at a culprit. */
#define GGA_NET_HASH_INTERVAL 60
#define GGA_NET_HASH_WINDOW 64

enum GGANetSessionRole {
	GGA_NET_SESSION_HOST = 0,
	GGA_NET_SESSION_GUEST = 1
};

/* Why a session ended, from GBASIONetSessionEndReason(). */
enum GGANetSessionEnd {
	GGA_NET_END_NONE = 0,
	GGA_NET_END_LOCAL = 1, /* we stopped it ourselves */
	GGA_NET_END_PEER_GONE = 2, /* the peer's socket died */
	GGA_NET_END_PEER_BYE = 3,
	GGA_NET_END_PEER_STOP = 4,
	GGA_NET_END_REJECT = 5,
	GGA_NET_END_DESYNC = 6,
	GGA_NET_END_PROTOCOL = 7
};

struct GBASIONetSession {
	int role;
	int localPlayer;
	int nPlayers;

	struct GBASIONetEndpoint* peer;
	int64_t (*nowMs)(void* context);
	void* context;
	int inputDeadlineMs;

	uint32_t frame; /* next frame to run */
	uint16_t localKeys;
	int64_t waitSinceMs; /* host: when the current frame started waiting */
	uint32_t guestHigh; /* host: highest frame the guest has announced */
	bool lagging; /* host: a frame ran without the guest's keys */
	bool paused;
	bool stopped;
	bool failed; /* protocol violation: stop and warn */
	bool desync; /* state hashes disagreed: stop and warn */
	uint8_t endReason; /* valid once ended; see GGANetSessionEnd */

	/* Sampled frames only, indexed by sample / INTERVAL % WINDOW. */
	uint32_t hashFrame[GGA_NET_HASH_WINDOW];
	uint32_t hashMine[GGA_NET_HASH_WINDOW][GGA_NET_MAX_PLAYERS];
	uint32_t hashTheirs[GGA_NET_HASH_WINDOW][GGA_NET_MAX_PLAYERS];
	bool hashMineValid[GGA_NET_HASH_WINDOW];
	bool hashTheirsValid[GGA_NET_HASH_WINDOW];

	/* Host: guest announcements, indexed by frame % WINDOW. */
	uint32_t guestFrame[GGA_NET_TICK_WINDOW];
	uint16_t guestKeys[GGA_NET_TICK_WINDOW];
	bool guestValid[GGA_NET_TICK_WINDOW];

	/* Guest: host ticks, indexed by frame % WINDOW. */
	uint32_t tickFrame[GGA_NET_TICK_WINDOW];
	uint16_t tickKeys[GGA_NET_TICK_WINDOW][GGA_NET_MAX_PLAYERS];
	uint8_t tickN[GGA_NET_TICK_WINDOW];
	bool tickValid[GGA_NET_TICK_WINDOW];
};

void GBASIONetSessionInit(struct GBASIONetSession* sess, int role, int localPlayer, int nPlayers,
	int64_t (*nowMs)(void* context), void* context);
/* Bind the endpoint for the single remote peer; also tells it to forward
 * INPUT/TICK/control frames here. */
void GBASIONetSessionSetPeer(struct GBASIONetSession* sess, struct GBASIONetEndpoint* peer);

/* Once both cores are reset and blobs applied. The guest announces its
 * first DELAY frames here so the host never has to idle at startup. */
void GBASIONetSessionStart(struct GBASIONetSession* sess);

/* The keys the local player is holding now, sampled when its frame runs. */
void GBASIONetSessionSetLocalInput(struct GBASIONetSession* sess, uint16_t keys);

/* True when frame `frame` may run: keys is filled with that frame's mask for
 * every player and the counter advances. False means "not yet, poll more" or
 * "the session is over" (see stopped/failed). */
bool GBASIONetSessionNextFrame(struct GBASIONetSession* sess, uint16_t* keys);
uint32_t GBASIONetSessionFrame(const struct GBASIONetSession* sess);

/* Report the per-player state hashes for the frame that just ran. Every
 * GGA_NET_HASH_INTERVAL frames this also sends them, and the peer's copy is
 * compared on arrival; a mismatch sets `desync`. Call after every frame, with
 * one hash per player, in player order. */
void GBASIONetSessionFrameDone(struct GBASIONetSession* sess, const uint32_t* hashes,
	uint8_t nPlayers);

/* True when the frame that NextFrame() just handed out is a sampling frame,
 * i.e. the caller should spend real state hashes on FrameDone. Hashing a
 * whole core is not cheap, so skip it on the frames that ignore the hashes. */
bool GBASIONetSessionSampling(const struct GBASIONetSession* sess);

/* Host: announce a pause/resume to the guest; both sides stop stepping
 * while paused. */
void GBASIONetSessionSetPaused(struct GBASIONetSession* sess, bool paused);
/* End the session. The host tells the guest first. */
void GBASIONetSessionStop(struct GBASIONetSession* sess, uint8_t reason);

/* Any of: stopped by either side, a protocol violation, or a desync. Once
 * ended, NextFrame() is false for good and the frontend should tear the
 * session down. */
bool GBASIONetSessionEnded(const struct GBASIONetSession* sess);
uint8_t GBASIONetSessionEndReason(const struct GBASIONetSession* sess);

/* Incoming wire frames, forwarded by the endpoint. */
void GBASIONetSessionOnInput(struct GBASIONetSession* sess, uint32_t frame, uint16_t keys);
void GBASIONetSessionOnTick(struct GBASIONetSession* sess, uint32_t frame,
	const uint16_t* keys, uint8_t nPlayers);
void GBASIONetSessionOnHash(struct GBASIONetSession* sess, uint32_t frame,
	const uint32_t* hashes, uint8_t nPlayers);
void GBASIONetSessionOnControl(struct GBASIONetSession* sess, uint8_t id, uint8_t reason);
/* The peer left: its endpoint died, or it said goodbye / was rejected.
 * `endReason` is the matching GGANetSessionEnd value. A leaving player ends
 * a 2-player session; keeping the rest running needs three or more and lands
 * with 4-player support. */
void GBASIONetSessionOnPeerGone(struct GBASIONetSession* sess, uint8_t endReason);

CXX_GUARD_END

#endif
