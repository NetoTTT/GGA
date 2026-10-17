/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/netrunner.h>

#include <mgba/internal/gba/sio/netsession.h>

#include <mgba/core/core.h>
#include <mgba/gba/interface.h>

#include <stdlib.h>
#include <string.h>

/* The park is the coordinator setting cpu->nextEvent = 0; this callback is
 * called with the coordinator lock held, so it must not run other cores. */
static void _park(struct mLockstepUser* u) {
	((struct GBASIONetRunnerCore*) u)->parked = true;
}

static void _wake(struct mLockstepUser* u) {
	((struct GBASIONetRunnerCore*) u)->parked = false;
}

static int _requestedId(struct mLockstepUser* u) {
	return ((struct GBASIONetRunnerCore*) u)->slot;
}

static void _idChanged(struct mLockstepUser* u, int id) {
	(void) u;
	(void) id;
}

void GBASIONetRunnerInit(struct GBASIONetRunner* runner, uint8_t nPlayers,
	struct GBASIONetSession* session) {
	memset(runner, 0, sizeof(*runner));
	runner->nPlayers = nPlayers;
	runner->session = session;
	GBASIOLockstepCoordinatorInit(&runner->coord);
}

bool GBASIONetRunnerAddCore(struct GBASIONetRunner* runner, uint8_t slot, struct mCore* core) {
	struct GBASIONetRunnerCore* c;
	if (slot >= runner->nPlayers || slot >= GGA_NET_MAX_PLAYERS || !core || runner->cores[slot].attached) {
		return false;
	}
	c = &runner->cores[slot];
	c->core = core;
	c->slot = slot;
	c->d.sleep = _park;
	c->d.wake = _wake;
	c->d.requestedId = _requestedId;
	c->d.playerIdChanged = _idChanged;
	GBASIOLockstepDriverCreate(&c->driver, &c->d);
	GBASIOLockstepCoordinatorAttach(&runner->coord, &c->driver);
	core->setPeripheral(core, mPERIPH_GBA_LINK_PORT, &c->driver.d);
	c->driver.d.reset(&c->driver.d);
	c->attached = true;
	return true;
}

/* Advance every attached core by exactly one frame. A parked (mid-transfer)
 * core is skipped and picked up once a peer wakes it, so the frame is only
 * complete when all cores have moved on -- which drains any in-flight
 * transfer as a side effect. */
static bool _runGroup(struct GBASIONetRunner* runner) {
	uint8_t i;
	unsigned long guard = 0;

	for (i = 0; i < runner->nPlayers; ++i) {
		struct GBASIONetRunnerCore* c = &runner->cores[i];
		if (!c->attached) {
			return false;
		}
		c->frameBase = c->core->frameCounter(c->core);
		c->done = false;
	}

	for (;;) {
		bool progressed = false;
		bool allDone = true;
		for (i = 0; i < runner->nPlayers; ++i) {
			struct GBASIONetRunnerCore* c = &runner->cores[i];
			if (c->done) {
				continue;
			}
			allDone = false;
			if (c->parked || c->running) {
				continue;
			}
			c->running = true;
			c->core->runLoop(c->core);
			c->running = false;
			progressed = true;
			if (c->core->frameCounter(c->core) != c->frameBase) {
				c->done = true;
			}
		}
		if (allDone) {
			return true;
		}
		if (!progressed) {
			/* Every remaining core is parked. Run one that already
			 * finished so it can wake them; a total park is fatal. */
			for (i = 0; i < runner->nPlayers; ++i) {
				struct GBASIONetRunnerCore* c = &runner->cores[i];
				if (c->parked || c->running) {
					continue;
				}
				c->running = true;
				c->core->runLoop(c->core);
				c->running = false;
				progressed = true;
				break;
			}
			if (!progressed) {
				return false;
			}
		}
		if (++guard > 2000000UL) {
			return false;
		}
	}
}

static uint32_t _hashCore(struct mCore* core) {
	size_t size = core->stateSize(core);
	uint32_t h = 0x811C9DC5u;
	size_t i;
	uint8_t* buf;

	if (!size) {
		return 0;
	}
	buf = malloc(size);
	if (!buf) {
		return 0;
	}
	memset(buf, 0, size);
	core->saveState(core, buf);
	for (i = 0; i < size; ++i) {
		h = (h ^ buf[i]) * 16777619u;
	}
	free(buf);
	return h;
}

void GBASIONetRunnerHashes(struct GBASIONetRunner* runner, uint32_t* hashes) {
	uint8_t i;
	for (i = 0; i < runner->nPlayers; ++i) {
		hashes[i] = runner->cores[i].attached ? _hashCore(runner->cores[i].core) : 0;
	}
}

bool GBASIONetRunnerStep(struct GBASIONetRunner* runner, uint16_t keys) {
	uint16_t masks[GGA_NET_MAX_PLAYERS];
	uint32_t hashes[GGA_NET_MAX_PLAYERS] = { 0 };
	bool sampling = false;
	uint8_t i;

	if (runner->session) {
		if (!GBASIONetSessionNextFrame(runner->session, masks)) {
			return false;
		}
		sampling = GBASIONetSessionSampling(runner->session);
	} else {
		for (i = 0; i < runner->nPlayers; ++i) {
			masks[i] = keys;
		}
	}

	for (i = 0; i < runner->nPlayers; ++i) {
		if (runner->cores[i].attached) {
			runner->cores[i].core->setKeys(runner->cores[i].core, masks[i]);
		}
	}
	if (!_runGroup(runner)) {
		return false;
	}

	if (runner->session) {
		if (sampling) {
			GBASIONetRunnerHashes(runner, hashes);
		}
		GBASIONetSessionFrameDone(runner->session, hashes, runner->nPlayers);
	}
	++runner->frames;
	return true;
}

void GBASIONetRunnerDeinit(struct GBASIONetRunner* runner) {
	uint8_t i;
	for (i = 0; i < runner->nPlayers; ++i) {
		struct GBASIONetRunnerCore* c = &runner->cores[i];
		if (!c->attached) {
			continue;
		}
		/* Clearing the peripheral deinits the lockstep driver, which also
		 * drops its player from the coordinator. */
		c->core->setPeripheral(c->core, mPERIPH_GBA_LINK_PORT, NULL);
		c->attached = false;
	}
	GBASIOLockstepCoordinatorDeinit(&runner->coord);
}
