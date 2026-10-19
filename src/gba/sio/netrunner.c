/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba/internal/gba/sio/netrunner.h>

#include <mgba/internal/gba/sio/netsession.h>

#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/timing.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/savedata.h>
#include <mgba/internal/gba/serialize.h>
#include <mgba/internal/arm/arm.h>

#include <mgba-util/image.h>
#include <mgba-util/vfs.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Diagnostics are opt-in. The probes below read core state next to the
 * scheduling they measure, so a normal run must not pay for them. */
static bool _diagEnabled(void) {
	static int enabled = -1;
	if (enabled < 0) {
		const char* value = getenv("GGA_DIAG");
		enabled = (value && *value) ? 1 : 0;
	}
	return enabled;
}

/* The park is the coordinator setting cpu->nextEvent = 0; this callback is
 * called with the coordinator lock held, so it must not run other cores. */
static void _park(struct mLockstepUser* u) {
	struct GBASIONetRunnerCore* c = (struct GBASIONetRunnerCore*) u;
	c->parked = true;
}

static void _wake(struct mLockstepUser* u) {
	struct GBASIONetRunnerCore* c = (struct GBASIONetRunnerCore*) u;
	c->parked = false;
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
	/* Every attached core is stepped from this one loop, in fixed slot order,
	 * so the slaves must not park on an empty queue: the park/wake pair
	 * leaves `cpu->nextEvent` at 0 and shifts the slave's event timing. */
	runner->coord.singleThreaded = true;
	/* The slave schedules its periodic lockstep event from the shared cycle,
	 * which the master drives from its own clock. Whichever core is local owns
	 * that clock on one machine and not on the other, so the slave's event
	 * timing came out a couple of cycles apart between the two machines.
	 * Re-anchor the slave to its own clock instead. */
	runner->coord.remoteClock = true;
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

struct _MirrorConfig {
	struct mCoreConfig* src;
	struct mCoreConfig* dst;
};

static void _mirrorConfigValue(const char* key, const char* value, enum mCoreConfigLevel type, void* user) {
	struct _MirrorConfig* ctx = user;
	(void) value;
	(void) type;
	mCoreConfigCopyValue(ctx->dst, ctx->src, key);
}

struct mCore* GBASIONetRunnerCreateMirrorCore(struct mCore* prototype, mColor* buffer, unsigned width) {
	struct GBA* src;
	struct mCore* core;
	if (!prototype || !prototype->board) {
		return NULL;
	}
	src = prototype->board;
	if (!src->memory.rom || !src->memory.romSize) {
		return NULL;
	}
	core = mCoreCreate(prototype->platform(prototype));
	if (!core) {
		return NULL;
	}
	core->init(core);
	mCoreInitConfig(core, NULL);
	{
		struct _MirrorConfig ctx = { &prototype->config, &core->config };
		mCoreConfigEnumerate(&prototype->config, NULL, _mirrorConfigValue, &ctx);
	}
	core->opts = prototype->opts;
	if (buffer) {
		core->setVideoBuffer(core, buffer, width);
	}
	mCoreLoadConfig(core);
	if (src->memory.bios) {
		struct VFile* biosVf = VFileFromConstMemory(src->memory.bios, 0x4000);
		if (biosVf) {
			core->loadBIOS(core, biosVf, 0);
		}
	}
	{
		struct VFile* romVf = VFileFromConstMemory(src->memory.rom, src->memory.romSize);
		if (!romVf || !core->loadROM(core, romVf)) {
			mCoreConfigDeinit(&core->config);
			core->deinit(core);
			return NULL;
		}
	}
	{
		size_t saveSize = GBASavedataSize(&src->memory.savedata);
		if (src->memory.savedata.data && saveSize) {
			struct VFile* saveVf = VFileMemChunk(src->memory.savedata.data, saveSize);
			if (saveVf) {
				core->loadSave(core, saveVf);
			}
		} else if (saveSize && src->memory.savedata.vf) {
			/* The local core's save is still in autodetect state, so its
			 * buffer has not been mapped yet. Clone the underlying file
			 * instead, or the mirror would start from an empty save and
			 * diverge as soon as the game probes the save hardware. */
			struct VFile* source = src->memory.savedata.vf;
			uint8_t* buffer = malloc(saveSize);
			if (buffer) {
				ssize_t read;
				source->seek(source, 0, SEEK_SET);
				read = source->read(source, buffer, saveSize);
				if (read > 0) {
					struct VFile* saveVf = VFileMemChunk(buffer, read);
					if (saveVf) {
						core->loadSave(core, saveVf);
					}
				}
				free(buffer);
			}
		}
	}
	if (src->rotationSource) {
		core->setPeripheral(core, mPERIPH_ROTATION, src->rotationSource);
	}
	if (src->luminanceSource) {
		core->setPeripheral(core, mPERIPH_GBA_LUMINANCE, src->luminanceSource);
	}
	if (src->rumble) {
		core->setPeripheral(core, mPERIPH_RUMBLE, src->rumble);
	}
	core->reset(core);
	return core;
}
/* Advance every attached core by exactly one frame. A parked (mid-transfer)
 * core is skipped and picked up once a peer wakes it. A core that cannot be
 * woken by any peer -- because the only other core has already finished its
 * own frame -- is released for the rest of the frame so it can run its own
 * frame out instead of waiting on a barrier that cannot arrive. */
static bool _runGroup(struct GBASIONetRunner* runner) {
	uint8_t i;
	unsigned long guard = 0;
	/* Run accounting, kept for the opt-in group log below. */
	uint32_t calls[GGA_NET_MAX_PLAYERS] = { 0 };
	uint32_t wakes[GGA_NET_MAX_PLAYERS] = { 0 };
	bool diag = false;

	for (i = 0; i < runner->nPlayers; ++i) {
		struct GBASIONetRunnerCore* c = &runner->cores[i];
		if (!c->attached) {
			return false;
		}
		c->frameBase = c->core->frameCounter(c->core);
		c->done = false;
		c->released = false;
	}

	for (;;) {
		struct GBASIONetRunnerCore* next = NULL;
		bool allDone = true;
		/* Step whichever core is due first in emulated time. Running one core
		 * out to its frame boundary and only then the other lets them drift
		 * apart by whole frames around a link transfer, which is exactly the
		 * window that has to stay synchronized. Both the ordering and the
		 * tie-break (lower slot wins) are pure functions of the emulated
		 * state, so every machine interleaves the same way. */
		for (i = 0; i < runner->nPlayers; ++i) {
			struct GBASIONetRunnerCore* c = &runner->cores[i];
			if (c->done) {
				continue;
			}
			allDone = false;
			if (c->running || (c->parked && !c->released)) {
				continue;
			}
			if (!next) {
				next = c;
				continue;
			}
			struct GBA* a = c->core->board;
			struct GBA* b = next->core->board;
			if ((int32_t) (a->cpu->nextEvent - b->cpu->nextEvent) < 0) {
				next = c;
			}
		}
		if (allDone) {
			for (i = 0; i < runner->nPlayers; ++i) {
				struct GBASIONetRunnerCore* c = &runner->cores[i];
				if (c->core->frameCounter(c->core) != c->frameBase + 1) {
					diag = true;
				}
			}
			if (_diagEnabled() && (diag || (runner->frames >= 118 && runner->frames <= 128))) {
				for (i = 0; i < runner->nPlayers; ++i) {
					struct GBASIONetRunnerCore* c = &runner->cores[i];
					fprintf(stderr,
						"DBG v2 grp f=%u s%u base=%u now=%u calls=%u wakes=%u park=%d\n",
						(unsigned) runner->frames, i, c->frameBase,
						c->core->frameCounter(c->core), calls[i], wakes[i],
						(int) c->parked);
				}
			}
			return true;
		}
		if (!next) {
			/* Everything left is parked, so no peer can reach the
			 * barrier that would wake it: the cores are waiting on
			 * each other. Release the lowest slot for the rest of this
			 * frame; that choice is a pure function of the emulated
			 * state, so both machines make the same one. */
			for (i = 0; i < runner->nPlayers; ++i) {
				struct GBASIONetRunnerCore* c = &runner->cores[i];
				if (c->done || !c->parked || c->running || c->released) {
					continue;
				}
				c->released = true;
				next = c;
				break;
			}
			if (!next) {
				if (_diagEnabled()) {
					fprintf(stderr, "DBG v2 group stall p0=%d p1=%d done0=%d done1=%d\n",
						(int) runner->cores[0].parked, (int) runner->cores[1].parked,
						(int) runner->cores[0].done, (int) runner->cores[1].done);
				}
				return false;
			}
		}
		if (next->parked) {
			/* Released above (an unreleased park is never picked). */
			GBASIOLockstepCoordinatorWakePlayer(&runner->coord, next->slot);
			next->parked = false;
			++wakes[next->slot];
		}
		next->running = true;
		next->core->runLoop(next->core);
		next->running = false;
		++calls[next->slot];
		if (next->core->frameCounter(next->core) != next->frameBase) {
			next->done = true;
		}
		if (++guard > 2000000UL) {
			if (_diagEnabled()) {
				fprintf(stderr, "DBG v2 group guard\n");
			}
			return false;
		}
	}
}

/* Hashes one core's canonical serialized state. */
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
	/* The resampled audio output buffers are filled by whichever frontend
	 * pulls samples, so they hold machine-local data that is never read by
	 * the game. Zeroing them keeps the divergence hash on emulated state. */
	{
		struct GBASerializedState* st = (struct GBASerializedState*) buf;
		memset(&st->samples, 0, sizeof(st->samples));
		memset(&st->currentSamples, 0, sizeof(st->currentSamples));
		/* The legacy PSG flag word carries the frame-sequencer step, which is
		 * derived from the timer divider phase, plus the envelope volumes,
		 * which only shape the audio output. The game-visible channel state
		 * is in the serialized IO registers, so this is not emulated state. */
		memset(&st->audio.flags, 0, sizeof(st->audio.flags));
	}
	for (i = 0; i < size; ++i) {
		h = (h ^ buf[i]) * 16777619u;
	}
	free(buf);
	return h;
}

/* Serializes one slot's current state and writes it out, for a
 * cross-machine byte diff driven from the frontend. */
void GBASIONetRunnerDumpNow(struct GBASIONetRunner* runner, uint8_t slot, const char* path) {
	uint8_t* buf;
	size_t size;
	FILE* f;

	if (slot >= runner->nPlayers || !runner->cores[slot].attached) {
		return;
	}
	size = runner->cores[slot].core->stateSize(runner->cores[slot].core);
	if (!size) {
		return;
	}
	buf = malloc(size);
	if (!buf) {
		return;
	}
	memset(buf, 0, size);
	runner->cores[slot].core->saveState(runner->cores[slot].core, buf);
	f = fopen(path, "wb");
	if (f) {
		fwrite(buf, 1, size, f);
		fclose(f);
	}
	free(buf);
}

void GBASIONetRunnerHashes(struct GBASIONetRunner* runner, uint32_t* hashes) {
	uint8_t i;
	for (i = 0; i < runner->nPlayers; ++i) {
		if (runner->cores[i].attached) {
			hashes[i] = _hashCore(runner->cores[i].core);
		} else {
			hashes[i] = 0;
		}
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
