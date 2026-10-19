/* GGA net bridge test: two real cores, two coordinators, in-process
 * transport between them. Host (slot 0) + client (slot 1).
 *
 * Build: same helper as link-harness (wsl-link-harness.sh pattern):
 *   gcc -g -O0 -DENABLE_VFS -DENABLE_DIRECTORIES -I include
 *     gga/link-netharness.c -L/home/foque/gga-build -lmgba
 *     -lSDL2 -lEGL -lGL -lz -lpng16 -lzip -lpthread -lm -ldl -o /tmp/netharness
 * Usage: netharness ROM BIOS SECONDS
 *
 * PASS: both cores run frames, wire messages flowed both ways, no
 * desync asserts, clean shutdown.
 */
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/internal/gba/sio/netdriver.h>
#include <mgba-util/threading.h>

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_INBOX 256

struct WireMsg {
	int senderSlot;
	int kind;
	int32_t a;
	int32_t b;
	uint8_t data[32];
	size_t len;
};

struct Side {
	struct mCore* core;
	struct GBASIOLockstepDriver driver;
	struct GBASIOLockstepCoordinator* coord;
	struct GBASIONetLink link;
	struct WireMsg inbox[MAX_INBOX];
	int inboxHead;
	int inboxTail;
	pthread_t thread;
	volatile bool stop;
	unsigned long frames;
	unsigned long targetFrames;
	Mutex mutex;
	Condition wakeCond;
	bool paused;
	long sentMsgs;
	long recvMsgs;
	struct Side* peer;
	int slot;
};

/* ---- blocking user (chains into net link) ---- */
struct ChainUser {
	struct mLockstepUser d;
	struct Side* side;
	int slot;
};

static void _bSleep(struct mLockstepUser* u) {
	struct Side* s = ((struct ChainUser*) u)->side;
	MutexLock(&s->mutex);
	s->paused = true;
	MutexUnlock(&s->mutex);
}

static void _bWake(struct mLockstepUser* u) {
	struct Side* s = ((struct ChainUser*) u)->side;
	MutexLock(&s->mutex);
	s->paused = false;
	ConditionWake(&s->wakeCond);
	MutexUnlock(&s->mutex);
}

static int _bRequestedId(struct mLockstepUser* u) {
	return ((struct ChainUser*) u)->slot;
}

static void* _runCore(void* ctx) {
	struct Side* s = ctx;
	while (!s->stop && s->frames < s->targetFrames) {
		MutexLock(&s->mutex);
		while (s->paused && !s->stop) {
			ConditionWait(&s->wakeCond, &s->mutex);
		}
		MutexUnlock(&s->mutex);
		if (s->stop) {
			break;
		}
		s->core->runFrame(s->core);
		++s->frames;
	}
	return 0;
}

/* ---- transport: enqueue into peer inbox (main-thread only) ---- */
static long s_wire[2][8];
static struct Side* s_hostSide;
static struct Side* s_clientSide;
static long s_h2c[8];
static long s_c2h[8];

static void _routeSend(void* ctx, int targetPlayer, int kind, int32_t a, int32_t b,
	const void* data, size_t len) {
	struct Side* dst = ctx;
	(void) targetPlayer;
	if (len > sizeof(dst->inbox[0].data)) {
		return;
	}
	if (dst == s_hostSide && kind >= 0 && kind < 8) {
		s_c2h[kind]++;
	} else if (kind >= 0 && kind < 8) {
		s_h2c[kind]++;
	}
	(void) a; (void) b;
	struct WireMsg* m = &dst->inbox[dst->inboxTail % MAX_INBOX];
	m->kind = kind;
	m->a = a;
	m->b = b;
	if (len && data) {
		memcpy(m->data, data, len);
	}
	m->len = len;
	dst->inboxTail++;
}

static int64_t _nowMs(void* ctx) {
	(void) ctx;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool _initSide(struct Side* s, const char* rom, const char* bios, int slot) {
	memset(s, 0, sizeof(*s));
	s->slot = slot;
	s->core = mCoreFind(rom);
	if (!s->core) {
		printf("FAIL: no core\n");
		return false;
	}
	s->core->init(s->core);
	struct VFile* biosVf = VFileOpen(bios, O_RDONLY);
	if (!biosVf || !s->core->loadBIOS(s->core, biosVf, 0)) {
		printf("FAIL: bios\n");
		return false;
	}
	if (!mCoreLoadFile(s->core, rom)) {
		printf("FAIL: rom\n");
		return false;
	}
	mCoreConfigInit(&s->core->config, "netharness");
	struct mCoreOptions opts = {};
	mCoreConfigMap(&s->core->config, &opts);
	opts.audioSync = false;
	opts.videoSync = false;
	mCoreConfigLoadDefaults(&s->core->config, &opts);
	mCoreLoadConfig(s->core);
	s->core->reset(s->core);
	MutexInit(&s->mutex);
	ConditionInit(&s->wakeCond);
	return true;
}

static void _nullLog(struct mLogger* log, int category, enum mLogLevel level, const char* format, va_list args) {
	(void) log; (void) category; (void) level; (void) format; (void) args;
}

int main(int argc, char** argv) {
	static struct mLogger nullLogger = { .log = _nullLog };
	mLogSetDefaultLogger(&nullLogger);
	fprintf(stderr, "NETHARNESS build tag 004\n");
	if (argc < 4) {
		printf("usage: %s ROM BIOS SECONDS\n", argv[0]);
		return 2;
	}
	int seconds = atoi(argv[3]);
	static struct Side host, client;
	static struct ChainUser hostUser, clientUser;
	if (!_initSide(&host, argv[1], argv[2], 0)) {
		return 1;
	}
	if (!_initSide(&client, argv[1], argv[2], 1)) {
		return 1;
	}
	host.targetFrames = client.targetFrames = (unsigned long) seconds * 60;

	/* Chain users (blocking) underneath the net link. */
	hostUser.side = &host;
	hostUser.slot = 0;
	hostUser.d.sleep = _bSleep;
	hostUser.d.wake = _bWake;
	hostUser.d.requestedId = _bRequestedId;
	hostUser.d.playerIdChanged = 0;
	clientUser.side = &client;
	clientUser.slot = 1;
	clientUser.d.sleep = _bSleep;
	clientUser.d.wake = _bWake;
	clientUser.d.requestedId = _bRequestedId;
	clientUser.d.playerIdChanged = 0;

	host.coord = calloc(1, sizeof(*host.coord));
	client.coord = calloc(1, sizeof(*client.coord));
	GBASIOLockstepCoordinatorInit(host.coord);
	GBASIOLockstepCoordinatorInit(client.coord);

	/* Transports route at each other (senderSlot filled at deliver). */
	struct GBASIONetTransport hostT = { &client, _routeSend, _nowMs, 0, 0 };
	struct GBASIONetTransport clientT = { &host, _routeSend, _nowMs, 0, 0 };

	GBASIOLockstepDriverCreate(&host.driver, &hostUser.d);
	GBASIONetLinkInit(&host.link, host.coord, &host.driver, &hostUser.d);
	host.link.transport = hostT;
	host.link.assignedSlot = 0;
	host.link.isHost = true;
	GBASIOLockstepCoordinatorAttach(host.coord, &host.driver);
	host.core->setPeripheral(host.core, mPERIPH_GBA_LINK_PORT, &host.driver.d);
	host.driver.d.reset(&host.driver.d);
	GBASIONetLinkAddPeer(&host.link, 1);

	GBASIOLockstepDriverCreate(&client.driver, &clientUser.d);
	GBASIONetLinkInit(&client.link, client.coord, &client.driver, &clientUser.d);
	client.link.transport = clientT;
	client.link.assignedSlot = 1;
	GBASIOLockstepCoordinatorAttach(client.coord, &client.driver);
	client.core->setPeripheral(client.core, mPERIPH_GBA_LINK_PORT, &client.driver.d);
	client.driver.d.reset(&client.driver.d);
	GBASIONetLinkAddPeer(&client.link, 0);

	pthread_create(&host.thread, 0, _runCore, &host);
	pthread_create(&client.thread, 0, _runCore, &client);

	/* Pump both links; deliver inboxes. */
	s_hostSide = &host;
	s_clientSide = &client;
	int spins = 0;
	while (spins < seconds * 200) {
		struct Side* sides[2] = { &host, &client };
		int s;
		for (s = 0; s < 2; ++s) {
			while (sides[s]->inboxHead != sides[s]->inboxTail) {
				struct WireMsg* m = &sides[s]->inbox[sides[s]->inboxHead % MAX_INBOX];
				int sender = (sides[s] == &host) ? 1 : 0;
				bool consumed = false;
				if (m->kind == GGA_NETW_TRANSFER_DATA && m->len >= 5) {
					uint32_t v = (uint32_t) m->data[1]
						| ((uint32_t) m->data[2] << 8)
						| ((uint32_t) m->data[3] << 16)
						| ((uint32_t) m->data[4] << 24);
					consumed = GBASIONetLinkOnTransferData(&sides[s]->link, m->data[0], v);
				} else {
					consumed = GBASIONetLinkOnWire(&sides[s]->link, sender, m->kind,
						m->a, m->b, m->data, m->len);
				}
				if (!consumed) {
					break;
				}
				sides[s]->recvMsgs++;
				sides[s]->inboxHead++;
			}
			GBASIONetLinkPoll(&sides[s]->link);
		}
		if ((spins % 10) == 0) {
			fprintf(stderr, "DBG t=%d hf=%lu cf=%lu hs=%d cs=%d hwait=%u cwait=%u hn=%d cn=%d hact=%d cact=%d\n",
			spins / 10, host.frames, client.frames, host.link.localSlot, client.link.localSlot,
			host.coord->waiting, client.coord->waiting,
			host.coord->nAttached, client.coord->nAttached,
			host.coord->transferActive, client.coord->transferActive);
		fprintf(stderr, "DBG h2c=%ld,%ld,%ld,%ld c2h=%ld,%ld,%ld,%ld\n",
			s_h2c[0], s_h2c[1], s_h2c[2], s_h2c[3],
			s_c2h[0], s_c2h[1], s_c2h[2], s_c2h[3]);
		}
		usleep(5000);
		++spins;
		if (host.frames >= host.targetFrames && client.frames >= client.targetFrames) {
			break;
		}
	}
	host.stop = client.stop = true;
	int spins2 = 0;
	while (spins2 < 200) {
		_bWake(&hostUser.d);
		_bWake(&clientUser.d);
		if (pthread_kill(host.thread, 0) != 0 && pthread_kill(client.thread, 0) != 0) {
			break;
		}
		usleep(50000);
		++spins2;
	}
	pthread_join(host.thread, 0);
	pthread_join(client.thread, 0);

	printf("host: frames=%lu rx=%ld\n", host.frames, host.recvMsgs);
	printf("client: frames=%lu rx=%ld\n", client.frames, client.recvMsgs);
	int ok = host.frames > 0 && client.frames > 0
		&& host.recvMsgs > 0 && client.recvMsgs > 0;
	printf(ok ? "NET-HARNESS PASS\n" : "NET-HARNESS FAIL\n");

	host.core->deinit(host.core);
	client.core->deinit(client.core);
	GBASIONetLinkDeinit(&host.link);
	GBASIONetLinkDeinit(&client.link);
	GBASIOLockstepCoordinatorDeinit(host.coord);
	GBASIOLockstepCoordinatorDeinit(client.coord);
	free(host.coord);
	free(client.coord);
	return ok ? 0 : 1;
}
