/* GGA 2-process link test: host and client over real TCP loopback.
 *
 * Build: same helper as link-netharness (wsl-netharness.sh pattern):
 *   gcc -g -O0 -DENABLE_VFS -DENABLE_DIRECTORIES -I include
 *     gga/link-netharness2.c -L/home/foque/gga-build -lmgba
 *     -lSDL2 -lEGL -lGL -lz -lpng16 -lzip -lpthread -lm -ldl -o /tmp/netharness2
 * Usage: netharness2 ROM BIOS SECONDS host|client
 * Host listens on 127.0.0.1:27182. Client attaches its driver only
 * after WELCOME (delayed attach, like the Qt frontend).
 */
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/internal/gba/sio/netdriver.h>
#include <mgba/internal/gba/sio/netep.h>
#include <mgba-util/threading.h>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct Proc {
	struct mCore* core;
	struct GBASIOLockstepDriver driver;
	struct GBASIOLockstepCoordinator* coord;
	struct GBASIONetLink link;
	struct GBASIONetEndpoint ep;
	struct mLockstepUser blockUser;
	pthread_t thread;
	volatile bool stop;
	volatile bool inFrame;
	unsigned long frames;
	unsigned long targetFrames;
	Mutex mutex;
	Condition wakeCond;
	bool paused;
	bool isHost;
	uint8_t romHash[32];
};

static void _bSleep(struct mLockstepUser* u) {
	struct Proc* p = (struct Proc*) ((char*) u - offsetof(struct Proc, blockUser));
	MutexLock(&p->mutex);
	p->paused = true;
	MutexUnlock(&p->mutex);
	fprintf(stderr, "SLEEP f=%lu slot=%d\n", p->frames, p->link.localSlot);
}

static void _bWake(struct mLockstepUser* u) {
	struct Proc* p = (struct Proc*) ((char*) u - offsetof(struct Proc, blockUser));
	MutexLock(&p->mutex);
	p->paused = false;
	ConditionWake(&p->wakeCond);
	MutexUnlock(&p->mutex);
	fprintf(stderr, "WAKE f=%lu slot=%d\n", p->frames, p->link.localSlot);
}

static int _bRequestedId(struct mLockstepUser* u) {
	struct Proc* p = (struct Proc*) ((char*) u - offsetof(struct Proc, blockUser));
	return p->link.assignedSlot;
}

static void* _runCore(void* ctx) {
	struct Proc* p = ctx;
	while (!p->stop && p->frames < p->targetFrames) {
		MutexLock(&p->mutex);
		while (p->paused && !p->stop) {
			ConditionWait(&p->wakeCond, &p->mutex);
		}
		MutexUnlock(&p->mutex);
		if (p->stop) {
			break;
		}
		p->inFrame = true;
		p->core->runFrame(p->core);
		++p->frames;
		p->inFrame = false;
	}
	return 0;
}

static int64_t _nowMs(void* ctx) {
	(void) ctx;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void _logNull(struct mLogger* log, int category, enum mLogLevel level, const char* format, va_list args) {
	(void) log; (void) category; (void) level; (void) format; (void) args;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 5) {
		printf("usage: %s ROM BIOS SECONDS host|client\n", argv[0]);
		return 2;
	}
	static struct mLogger nullLogger = { .log = _logNull };


	bool isHost = !strcmp(argv[4], "host");
	int seconds = atoi(argv[3]);
	static struct Proc p;
	memset(&p, 0, sizeof(p));
	p.isHost = isHost;
	p.targetFrames = (unsigned long) seconds * 60;

	p.core = mCoreFind(argv[1]);
	if (!p.core) {
		printf("FAIL: no core\n");
		return 1;
	}
	p.core->init(p.core);
	struct VFile* biosVf = VFileOpen(argv[2], O_RDONLY);
	if (!biosVf || !p.core->loadBIOS(p.core, biosVf, 0)) {
		printf("FAIL: bios\n");
		return 1;
	}
	if (!mCoreLoadFile(p.core, argv[1])) {
		printf("FAIL: rom\n");
		return 1;
	}
	mCoreConfigInit(&p.core->config, "netharness2");
	struct mCoreOptions opts = {};
	mCoreConfigMap(&p.core->config, &opts);
	opts.audioSync = false;
	opts.videoSync = false;
	mCoreConfigLoadDefaults(&p.core->config, &opts);
	mCoreLoadConfig(p.core);
	p.core->reset(p.core);
	MutexInit(&p.mutex);
	ConditionInit(&p.wakeCond);

	/* ROM identity: first 32 bytes hashed simply (v0 compat check). */
	{
		struct VFile* rom = VFileOpen(argv[1], O_RDONLY);
		uint8_t head[64] = { 0 };
		if (rom) {
			rom->read(rom, head, sizeof(head));
			rom->close(rom);
		}
		uint32_t h = 0x811C9DC5u;
		size_t i;
		for (i = 0; i < sizeof(head); ++i) {
			h = (h ^ head[i]) * 16777619u;
		}
		memset(p.romHash, 0, sizeof(p.romHash));
		p.romHash[0] = h & 0xFF;
		p.romHash[1] = (h >> 8) & 0xFF;
		p.romHash[2] = (h >> 16) & 0xFF;
		p.romHash[3] = (h >> 24) & 0xFF;
	}

	p.coord = calloc(1, sizeof(*p.coord));
	GBASIOLockstepCoordinatorInit(p.coord);

	struct GBASIONetTransport frontend = { 0, 0, _nowMs, 0, 0 };
	p.blockUser.sleep = _bSleep;
	p.blockUser.wake = _bWake;
	p.blockUser.requestedId = _bRequestedId;
	p.blockUser.playerIdChanged = 0;

	GBASIOLockstepDriverCreate(&p.driver, &p.blockUser);
	GBASIONetLinkInit(&p.link, p.coord, &p.driver, &p.blockUser);
	GBASIONetEndpointInit(&p.ep, &p.link, &frontend);
	GBASIONetEndpointSetIdentity(&p.ep, p.romHash, isHost ? "P0" : "P1");

	if (isHost) {
		p.link.assignedSlot = 0;
		p.link.isHost = true;
		if (!GBASIONetEndpointListen(&p.ep, 27182)) {
			fprintf(stderr, "FAIL: listen\n");
			return 1;
		}
		GBASIOLockstepCoordinatorAttach(p.coord, &p.driver);
		p.core->setPeripheral(p.core, mPERIPH_GBA_LINK_PORT, &p.driver.d);
		p.driver.d.reset(&p.driver.d);
		GBASIONetLinkAddPeer(&p.link, 1);
		fprintf(stderr, "HOST listening\n");
	} else {
		if (!GBASIONetEndpointConnect(&p.ep, "127.0.0.1", 27182)) {
			fprintf(stderr, "FAIL: connect\n");
			return 1;
		}
		fprintf(stderr, "CLIENT connected, waiting for WELCOME\n");
		int spins = 0;
		while (!p.ep.welcomed && spins < 2000) {
			GBASIONetEndpointPoll(&p.ep);
			usleep(5000);
			++spins;
			if ((spins % 200) == 0) {
				fprintf(stderr, "WAIT spins=%d welcomed=%d\n",
					spins, p.ep.welcomed);
			}
		}
		if (!p.ep.welcomed) {
			fprintf(stderr, "FAIL: no welcome (slot=%d)\n", p.link.assignedSlot);
			return 1;
		}
		fprintf(stderr, "CLIENT welcomed as slot %d\n", p.link.assignedSlot);
		GBASIOLockstepCoordinatorAttach(p.coord, &p.driver);
		p.core->setPeripheral(p.core, mPERIPH_GBA_LINK_PORT, &p.driver.d);
		p.driver.d.reset(&p.driver.d);
		GBASIONetLinkAddPeer(&p.link, 0);
	}

	pthread_create(&p.thread, 0, _runCore, &p);
	FILE* spinlog = fopen("/tmp/spin.log", "w");
	int spins = 0;
	while (spins < seconds * 200) {
		fputc('.', spinlog);
		if ((spins % 200) == 0) { fflush(spinlog); }
		GBASIONetEndpointPoll(&p.ep);
		usleep(5000);
		++spins;
		if (p.frames >= p.targetFrames) {
			break;
		}
		if ((spins % 200) == 0) {
			int held = MutexTryLock(&p.coord->mutex) ? 0 : 1;
			if (!held) {
				MutexUnlock(&p.coord->mutex);
			}
			fprintf(stderr, "DBG f=%lu slot=%d inf=%d bp=%d as=%d def=%zu ls=%d mt=%d wait=0x%X tx=%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld rx=%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld\n", p.frames, p.link.localSlot, p.inFrame,
		p.link.barrierPending, p.link.ackSeen, p.ep.nDeferred, p.link.localSlot, held, p.coord->waiting,
		p.ep.statTx[0], p.ep.statTx[1], p.ep.statTx[2], p.ep.statTx[3],
		p.ep.statTx[4], p.ep.statTx[5], p.ep.statTx[6], p.ep.statTx[7],
		p.ep.statRx[0], p.ep.statRx[1], p.ep.statRx[2], p.ep.statRx[3],
		p.ep.statRx[4], p.ep.statRx[5], p.ep.statRx[6], p.ep.statRx[7]);
		}
	}
	p.stop = true;
	int spin2 = 0;
	while (spin2 < 100) {
		_bWake(&p.blockUser);
		GBASIONetEndpointPoll(&p.ep);
		if (pthread_kill(p.thread, 0) != 0) {
			break;
		}
		usleep(50000);
		++spin2;
	}
	pthread_join(p.thread, 0);
	fprintf(stderr, "DONE frames=%lu slot=%d\n", p.frames, p.link.localSlot);
	int ok = p.frames > 0;
	printf(ok ? "NETHARNESS2 PASS\n" : "NETHARNESS2 FAIL\n");

	p.core->deinit(p.core);
	GBASIONetLinkDeinit(&p.link);
	GBASIOLockstepCoordinatorDeinit(p.coord);
	free(p.coord);
	GBASIONetEndpointClose(&p.ep);
	return ok ? 0 : 1;
}
