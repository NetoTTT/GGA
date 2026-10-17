/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_NETRUNNER_H
#define GBA_SIO_NETRUNNER_H

/* Local side of a v2 session: every player's core runs in this process and
 * they advance one frame at a time in lockstep. The network only carries key
 * masks. See docs/NETPLAY-ARCHITECTURE.md.
 *
 * A core that is mid-transfer is parked by the coordinator and picked up on
 * a later pass, so this is a cooperative loop: Step() must be called from
 * one thread only, and the sleep callback never runs other cores.
 */
#include <mgba-util/common.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/internal/gba/sio/net.h>

CXX_GUARD_START

struct GBASIONetSession;
struct mCore;

struct GBASIONetRunnerCore {
	struct mLockstepUser d; /* must be first: the coordinator casts back */
	struct GBASIOLockstepDriver driver;
	struct mCore* core;
	uint8_t slot;
	uint32_t frameBase;
	bool attached;
	bool done;
	bool running;
	bool parked;
};

struct GBASIONetRunner {
	struct GBASIONetSession* session;
	struct GBASIOLockstepCoordinator coord;
	struct GBASIONetRunnerCore cores[GGA_NET_MAX_PLAYERS];
	uint8_t nPlayers;
	uint64_t frames;
};

/* `session` may be NULL: Step() then feeds `keys` to every core, which is
 * what a local-only run wants. */
void GBASIONetRunnerInit(struct GBASIONetRunner* runner, uint8_t nPlayers,
	struct GBASIONetSession* session);
/* Attach one player's core (already loaded, configured and reset). */
bool GBASIONetRunnerAddCore(struct GBASIONetRunner* runner, uint8_t slot, struct mCore* core);
/* Advance every core by exactly one frame. With a session set its masks win
 * and its hashes are reported; otherwise every core gets `keys`. False means
 * the session is over (or the frame group could not finish). */
bool GBASIONetRunnerStep(struct GBASIONetRunner* runner, uint16_t keys);
/* State hashes for the frame that just ran, one per player. This is the
 * divergence signal, and it saves a whole core state per player, so call it
 * on sampling frames only. */
void GBASIONetRunnerHashes(struct GBASIONetRunner* runner, uint32_t* hashes);
/* Detach every core. The cores themselves stay the caller's to destroy. */
void GBASIONetRunnerDeinit(struct GBASIONetRunner* runner);

CXX_GUARD_END

#endif
