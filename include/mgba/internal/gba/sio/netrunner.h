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
#include <mgba-util/image.h>
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
	bool released;
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
/* Build a core that mirrors prototype as far as the emulation can observe:
 * same platform, BIOS, ROM, save, config values, options and peripherals. A
 * linked core built any other way can diverge on the first link handshake even
 * though its serialized state is identical, so frontends should create every
 * extra player core through this instead of building one by hand. `buffer` may
 * be NULL; when set it is the video buffer the core renders into (kept by the
 * caller). Returns a core that the caller owns, or NULL. */
struct mCore* GBASIONetRunnerCreateMirrorCore(struct mCore* prototype, mColor* buffer, unsigned width);
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

/* Serialize one slot's current state and write it to `path`. Used by the LAN
 * control server for a cross-machine byte diff. */
void GBASIONetRunnerDumpNow(struct GBASIONetRunner* runner, uint8_t slot, const char* path);

CXX_GUARD_END

#endif
