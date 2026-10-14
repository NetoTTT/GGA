/* GGA link harness v0: two headless GBA cores on one lockstep coordinator.
 *
 * Proves coordinator + drivers + threads integrate with real cores
 * (attach, ids, frames, no desync asserts). No gameplay input yet, so
 * no SIO transfers are expected — transfer tests come next.
 *
 * Build (Linux):
 *   gcc -I include gga/link-harness.c <mgba objects...> -o /tmp/link-harness -pthread
 * Easiest: compile against the built static objects via the helper
 * script docs/wsl-link-harness.sh, which links the exact objects the
 * SDL frontend uses (see below).
 *
 * Usage: link-harness ROM BIOS SECONDS
 */
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba-util/threading.h>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct HarnessUser {
	struct mLockstepUser d;
	int slot;
	int id;
	Mutex mutex;
	Condition wakeCond;
	bool woken;
};

struct HarnessCore {
	struct mCore* core;
	struct GBASIOLockstepDriver driver;
	struct HarnessUser user;
	pthread_t thread;
	volatile bool stop;
	unsigned long frames;
	unsigned long targetFrames;
};

static void _harnessSleep(struct mLockstepUser* user) {
	struct HarnessUser* hu = (struct HarnessUser*) user;
	fprintf(stderr, "DBG sleep enter\n");
	MutexLock(&hu->mutex);
	while (!hu->woken) {
		ConditionWait(&hu->wakeCond, &hu->mutex);
	}
	hu->woken = false;
	MutexUnlock(&hu->mutex);
	fprintf(stderr, "DBG sleep leave\n");
}

static void _harnessWake(struct mLockstepUser* user) {
	struct HarnessUser* hu = (struct HarnessUser*) user;
	fprintf(stderr, "DBG wake\n");
	MutexLock(&hu->mutex);
	hu->woken = true;
	ConditionWake(&hu->wakeCond);
	MutexUnlock(&hu->mutex);
}

static int _harnessRequestedId(struct mLockstepUser* user) {
	struct HarnessUser* hu = (struct HarnessUser*) user;
	return hu->slot;
}

static void _harnessIdChanged(struct mLockstepUser* user, int id) {
	struct HarnessUser* hu = (struct HarnessUser*) user;
	hu->id = id;
}

static void* _runCore(void* ctx) {
	struct HarnessCore* hc = ctx;
	fprintf(stderr, "DBG run start stop=%d target=%lu\n", hc->stop, hc->targetFrames);
	while (!hc->stop && hc->frames < hc->targetFrames) {
		hc->core->runFrame(hc->core);
		++hc->frames;
	}
	fprintf(stderr, "DBG thread exit frames=%lu\n", hc->frames);
	return 0;
}

static bool _initCore(struct HarnessCore* hc, const char* rom, const char* bios) {
	memset(hc, 0, sizeof(*hc));
	hc->core = mCoreFind(rom);
	if (!hc->core) {
		printf("FAIL: no core for %s\n", rom);
		return false;
	}
	hc->core->init(hc->core);
	struct VFile* biosVf = VFileOpen(bios, O_RDONLY);
	if (!biosVf) {
		printf("FAIL: no bios %s\n", bios);
		return false;
	}
	if (!hc->core->loadBIOS(hc->core, biosVf, 0)) {
		printf("FAIL: bios rejected\n");
		return false;
	}
	if (!mCoreLoadFile(hc->core, rom)) {
		printf("FAIL: rom rejected\n");
		return false;
	}
	mCoreConfigInit(&hc->core->config, "linkharness");
	struct mCoreOptions opts = {};
	mCoreConfigMap(&hc->core->config, &opts);
	opts.audioSync = false;
	opts.videoSync = false;
	mCoreConfigLoadDefaults(&hc->core->config, &opts);
	mCoreLoadConfig(hc->core);
	hc->core->reset(hc->core);
	fprintf(stderr, "DBG init done core=%p setPeripheral=%p\n",
		(void*) hc->core, (void*) hc->core->setPeripheral);
	return true;
}

int main(int argc, char** argv) {
	if (argc < 4) {
		printf("usage: %s ROM BIOS SECONDS\n", argv[0]);
		return 2;
	}
	int seconds = atoi(argv[3]);
	struct HarnessCore cores[2];
	int i;
	for (i = 0; i < 2; ++i) {
		if (!_initCore(&cores[i], argv[1], argv[2])) {
			return 1;
		}
		cores[i].targetFrames = (unsigned long) seconds * 60;
		cores[i].user.slot = i;
		cores[i].user.id = -1;
		cores[i].user.d.sleep = _harnessSleep;
		cores[i].user.d.wake = _harnessWake;
		cores[i].user.d.requestedId = _harnessRequestedId;
		cores[i].user.d.playerIdChanged = _harnessIdChanged;
		MutexInit(&cores[i].user.mutex);
		ConditionInit(&cores[i].user.wakeCond);
	}

	struct GBASIOLockstepCoordinator* coord = calloc(1, sizeof(*coord));
	fprintf(stderr, "DBG sizeof=%zu c0=%p c1=%p\n", sizeof(struct HarnessCore),
		(void*) &cores[0], (void*) &cores[1]);
	fprintf(stderr, "DBG after-init core0=%p core1=%p\n",
		(void*) cores[0].core, (void*) cores[1].core);
	GBASIOLockstepCoordinatorInit(coord);
	for (i = 0; i < 2; ++i) {
		fprintf(stderr, "DBG attach %d core=%p\n", i, (void*) cores[i].core);
		GBASIOLockstepDriverCreate(&cores[i].driver, &cores[i].user.d);
		GBASIOLockstepCoordinatorAttach(coord, &cores[i].driver);
		cores[i].core->setPeripheral(cores[i].core, mPERIPH_GBA_LINK_PORT, &cores[i].driver.d);
		cores[i].driver.d.reset(&cores[i].driver.d);
		pthread_create(&cores[i].thread, 0, _runCore, &cores[i]);
		fprintf(stderr, "DBG thread %d started\n", i);
	}

	sleep(seconds);
	fprintf(stderr, "DBG sleep done, stopping\n");
	for (i = 0; i < 2; ++i) {
		cores[i].stop = true;
	}
	/* Wake repeatedly: a thread may re-sleep between our wake and join. */
	int spins = 0;
	while (spins < 100) {
		int alive = 0;
		for (i = 0; i < 2; ++i) {
			_harnessWake(&cores[i].user.d);
			if (pthread_kill(cores[i].thread, 0) == 0) {
				++alive;
			}
		}
		if (!alive) {
			break;
		}
		usleep(50000);
		++spins;
	}
	fprintf(stderr, "DBG joining\n");
	for (i = 0; i < 2; ++i) {
		pthread_join(cores[i].thread, 0);
		fprintf(stderr, "DBG joined %d\n", i);
	}

	int ok = 1;
	for (i = 0; i < 2; ++i) {
		int devId = cores[i].driver.d.deviceId(&cores[i].driver.d);
		int conn = cores[i].driver.d.connectedDevices(&cores[i].driver.d);
		printf("core%d: frames=%lu playerId=%d deviceId=%d connected=%d\n",
			i, cores[i].frames, cores[i].user.id, devId, conn);
		if (cores[i].frames == 0) {
			ok = 0;
		}
		cores[i].core->deinit(cores[i].core);
	}
	GBASIOLockstepCoordinatorDeinit(coord); free(coord);
	printf(ok ? "LINK-HARNESS PASS\n" : "LINK-HARNESS FAIL\n");
	return ok ? 0 : 1;
}
