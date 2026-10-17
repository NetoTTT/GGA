/* GGA input-netplay session skeleton (Phase 1c, step 1).
 *
 * Runs N headless GBA cores plus one GBASIOLockstepCoordinator in a
 * SINGLE thread and advances them deterministically one frame at a
 * time, with per-player input masks. This is the local half of the
 * input-netplay model (docs/NETPLAY-ARCHITECTURE.md): the cable is
 * simulated locally by the core, and the eventual network layer will
 * only carry inputs and state hashes. No network here.
 *
 * It proves three things the phase depends on:
 *   1. cooperative single-thread lockstep (a non-blocking mLockstepUser
 *      whose sleep() merely parks a core, so the runner can interleave);
 *   2. local determinism: the same run twice yields identical state
 *      hashes (a deterministic RTC seed is applied before reset);
 *   3. frame drain and per-player state hashing.
 *
 * Build (Linux/WSL): scripts/wsl-build-linksession.sh (mod-coop helper)
 * Usage: link-session ROM BIOS FRAMES [KEYMASK_HEX] [--twice] [--nolink]
 */
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/sio/lockstep.h>

#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

static void _quietLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	UNUSED(category);
	UNUSED(level);
	UNUSED(format);
	UNUSED(args);
}

static struct mLogger _quietLogger = { .log = _quietLog };

static bool g_noLink;

#define SESSION_MAX 4

struct Session;
struct SessionCore;

struct SessionUser {
	struct mLockstepUser d; /* must be first: the coordinator casts back */
	struct Session* sess;
	struct SessionCore* sc;
	int slot;
	int id;
	bool parked;
	bool running;
};

struct SessionCore {
	struct mCore* core;
	struct GBASIOLockstepDriver driver;
	struct SessionUser user;
	uint32_t frameBase;
	bool done;
};

struct Session {
	int n;
	struct SessionCore c[SESSION_MAX];
	struct GBASIOLockstepCoordinator coord;
	/* stats */
	unsigned long frames;
	unsigned long sleeps;
	unsigned long wakes;
};

static void _sessSleep(struct mLockstepUser* u) {
	struct SessionUser* su = (struct SessionUser*) u;
	/* Non-blocking, like upstream's thread user: the park itself is the
	 * coordinator setting cpu->nextEvent = 0, so this callback must not
	 * run other cores (the coordinator mutex is held here). The frame
	 * loop drives the peers instead. */
	su->parked = true;
	++su->sess->sleeps;
}

static void _sessWake(struct mLockstepUser* u) {
	struct SessionUser* su = (struct SessionUser*) u;
	su->parked = false;
	++su->sess->wakes;
}

static int _sessRequestedId(struct mLockstepUser* u) {
	struct SessionUser* su = (struct SessionUser*) u;
	return su->slot;
}

static void _sessIdChanged(struct mLockstepUser* u, int id) {
	struct SessionUser* su = (struct SessionUser*) u;
	su->id = id;
}

static bool _initCore(struct SessionCore* sc, struct Session* s, int slot,
	const char* rom, const char* bios) {
	struct mCore* core = mCoreFind(rom);
	if (!core) {
		fprintf(stderr, "FAIL: no core for %s\n", rom);
		return false;
	}
	core->init(core);
	struct VFile* biosVf = VFileOpen(bios, O_RDONLY);
	if (!biosVf || !core->loadBIOS(core, biosVf, 0)) {
		fprintf(stderr, "FAIL: bios rejected (%s)\n", bios);
		return false;
	}
	if (!mCoreLoadFile(core, rom)) {
		fprintf(stderr, "FAIL: rom rejected (%s)\n", rom);
		return false;
	}
	mCoreConfigInit(&core->config, "linksession");
	struct mCoreOptions opts = {};
	mCoreConfigMap(&core->config, &opts);
	opts.audioSync = false;
	opts.videoSync = false;
	mCoreConfigLoadDefaults(&core->config, &opts);
	mCoreLoadConfig(core);

	/* Deterministic RTC: a hidden input if left to the wall clock. */
	core->rtc.override = RTC_FAKE_EPOCH;
	core->rtc.value = 1200000000000LL / 1000; /* fixed epoch, milliseconds */
	core->reset(core);

	memset(sc, 0, sizeof(*sc));
	sc->core = core;
	sc->user.sess = s;
	sc->user.sc = sc;
	sc->user.slot = slot;
	sc->user.id = -1;
	sc->user.parked = false;
	sc->user.d.sleep = _sessSleep;
	sc->user.d.wake = _sessWake;
	sc->user.d.requestedId = _sessRequestedId;
	sc->user.d.playerIdChanged = _sessIdChanged;
	return true;
}

static void _attach(struct Session* s, int i) {
	struct SessionCore* sc = &s->c[i];
	GBASIOLockstepDriverCreate(&sc->driver, &sc->user.d);
	GBASIOLockstepCoordinatorAttach(&s->coord, &sc->driver);
	sc->core->setPeripheral(sc->core, mPERIPH_GBA_LINK_PORT, &sc->driver.d);
	sc->driver.d.reset(&sc->driver.d);
}

/* Advance every core by exactly one frame. A core that is mid-transfer
 * is parked by the coordinator and simply skipped until a wake; the
 * frame only completes once all cores have moved on, which drains any
 * in-flight transfer as a side effect. */
static bool _runFrameGroup(struct Session* s, uint32_t keys) {
	int i;
	unsigned long guard = 0;
	bool allDone;

	for (i = 0; i < s->n; ++i) {
		struct SessionCore* sc = &s->c[i];
		sc->core->setKeys(sc->core, keys);
		sc->frameBase = sc->core->frameCounter(sc->core);
		sc->done = false;
	}

	do {
		bool progressed = false;
		allDone = true;
		for (i = 0; i < s->n; ++i) {
			struct SessionCore* sc = &s->c[i];
			if (sc->done) {
				continue;
			}
			allDone = false;
			if (sc->user.parked || sc->user.running) {
				continue;
			}
			sc->user.running = true;
			sc->core->runLoop(sc->core);
			sc->user.running = false;
			progressed = true;
			if (sc->core->frameCounter(sc->core) != sc->frameBase) {
				sc->done = true;
			}
		}
		if (allDone) {
			break;
		}
		if (!progressed) {
			/* Every remaining core is parked. Run a peer once (even a
			 * finished one) so it can wake them; a total park is fatal. */
			int k;
			for (k = 0; k < s->n; ++k) {
				struct SessionCore* sc = &s->c[k];
				if (sc->user.parked || sc->user.running) {
					continue;
				}
				sc->user.running = true;
				sc->core->runLoop(sc->core);
				sc->user.running = false;
				progressed = true;
				break;
			}
			if (!progressed) {
				fprintf(stderr, "FAIL: deadlock at frame %lu\n", s->frames);
				return false;
			}
		}
		if (++guard > 2000000UL) {
			fprintf(stderr, "FAIL: livelock guard at frame %lu\n", s->frames);
			return false;
		}
		if (guard % 100000UL == 0) {
			fprintf(stderr, "FRAME f=%lu g=%lu waiting=%x tActive=%d [0 p=%d d=%d] [1 p=%d d=%d]\n",
				s->frames, guard, s->coord.waiting, s->coord.transferActive,
				s->c[0].user.parked, s->c[0].done, s->c[1].user.parked, s->c[1].done);
		}
	} while (true);

	++s->frames;
	return true;
}

static uint32_t _stateHash(struct mCore* core) {
	size_t size = core->stateSize(core);
	uint8_t* buf = malloc(size);
	if (!buf) {
		return 0;
	}
	memset(buf, 0, size);
	core->saveState(core, buf);
	uint32_t h = 0x811C9DC5u;
	size_t i;
	for (i = 0; i < size; ++i) {
		h = (h ^ buf[i]) * 16777619u;
	}
	free(buf);
	return h;
}

static bool _runOnce(const char* rom, const char* bios, unsigned frames, uint32_t keys,
	uint32_t out[SESSION_MAX], struct Session* outStats) {
	static struct Session s;
	memset(&s, 0, sizeof(s));
	GBASIOLockstepCoordinatorInit(&s.coord);

	s.n = 2;
	int i;
	for (i = 0; i < s.n; ++i) {
		if (!_initCore(&s.c[i], &s, i, rom, bios)) {
			return false;
		}
	}
	for (i = 0; i < s.n; ++i) {
		if (!g_noLink) {
			_attach(&s, i);
		}
	}

	unsigned f;
	for (f = 0; f < frames; ++f) {
		if (!_runFrameGroup(&s, keys)) {
			return false;
		}
	}

	for (i = 0; i < s.n; ++i) {
		out[i] = _stateHash(s.c[i].core);
	}
	if (outStats) {
		*outStats = s;
	}

	for (i = 0; i < s.n; ++i) {
		s.c[i].core->deinit(s.c[i].core);
	}
	GBASIOLockstepCoordinatorDeinit(&s.coord);
	return true;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	mLogSetDefaultLogger(&_quietLogger);
	if (argc < 4) {
		printf("usage: %s ROM BIOS FRAMES [KEYMASK_HEX] [--twice] [--nolink]\n", argv[0]);
		return 2;
	}
	const char* rom = argv[1];
	const char* bios = argv[2];
	unsigned frames = (unsigned) strtoul(argv[3], NULL, 10);
	uint32_t keys = 0;
	bool twice = false;
	int i;
	for (i = 4; i < argc; ++i) {
		if (!strcmp(argv[i], "--twice")) {
			twice = true;
		} else if (!strcmp(argv[i], "--nolink")) {
			g_noLink = true;
		} else {
			keys = (uint32_t) strtoul(argv[i], NULL, 16);
		}
	}

	uint32_t h1[SESSION_MAX] = {0};
	uint32_t h2[SESSION_MAX] = {0};
	struct Session stats;
	memset(&stats, 0, sizeof(stats));

	if (!_runOnce(rom, bios, frames, keys, h1, &stats)) {
		printf("LINK-SESSION FAIL (run 1)\n");
		return 1;
	}
	printf("run1: frames=%lu sleeps=%lu wakes=%lu\n",
		stats.frames, stats.sleeps, stats.wakes);
	for (i = 0; i < 2; ++i) {
		printf("  player%d hash=%08" PRIX32 "\n", i, h1[i]);
	}

	bool ok = true;
	if (twice) {
		if (!_runOnce(rom, bios, frames, keys, h2, NULL)) {
			printf("LINK-SESSION FAIL (run 2)\n");
			return 1;
		}
		printf("run2:\n");
		for (i = 0; i < 2; ++i) {
			printf("  player%d hash=%08" PRIX32 "%s\n", i, h2[i],
				h2[i] == h1[i] ? "" : "  <-- DIFFERS");
			if (h2[i] != h1[i]) {
				ok = false;
			}
		}
	}

	printf(ok ? "LINK-SESSION PASS\n" : "LINK-SESSION FAIL (nondeterministic)\n");
	return ok ? 0 : 1;
}
