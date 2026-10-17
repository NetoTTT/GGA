/* GBASIONetRunner: the local half of a v2 session. Two players' cores run in
 * one thread, one frame at a time, driven either by a fixed mask (local
 * only) or by a GBASIONetSession fed over the wire.
 *
 * Phase A proves the frame group: both cores advance together, a real MULTI
 * transfer survives, and two identical runs hash the same.
 * Phase B is the whole point: two independent clients, each running BOTH
 * players' cores, exchange only key masks and must stay byte-identical.
 *
 * Usage: netrunner-test ROM BIOS
 */
#include <mgba/core/core.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio.h>
#include <mgba/internal/gba/sio/lockstep.h>

#include <mgba/internal/gba/sio/netrunner.h>
#include <mgba/internal/gba/sio/netsession.h>
#include <mgba/internal/gba/sio/netep.h>
#include <mgba/internal/gba/sio/netdriver.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PORT 27189
#define LOCAL_FRAMES 120
#define RUN_FRAMES 200

static const char* g_rom;
static const char* g_bios;
static int64_t g_now;

static struct GBASIOLockstepCoordinator g_hc, g_gc;
static struct GBASIOLockstepDriver g_hd, g_gd;
static struct GBASIONetLink g_hl, g_gl;
static struct GBASIONetTransport g_transport;

static void _quietLog(struct mLogger* logger, int category, enum mLogLevel level, const char* format, va_list args) {
	UNUSED(logger);
	UNUSED(category);
	UNUSED(level);
	UNUSED(format);
	UNUSED(args);
}

static struct mLogger _quietLogger = { .log = _quietLog };

static int64_t _nowMs(void* context) {
	(void) context;
	return g_now;
}

static bool _fail(const char* what, int line) {
	printf("FAIL line %d: %s\n", line, what);
	return false;
}
#define CHECK(c) do { if (!(c)) { return _fail(#c, __LINE__); } } while (0)

static struct mCore* _loadCore(void) {
	struct mCore* core = mCoreFind(g_rom);
	struct VFile* bios;
	struct mCoreOptions opts = {};
	if (!core) {
		printf("FAIL: no core for %s\n", g_rom);
		return NULL;
	}
	core->init(core);
	bios = VFileOpen(g_bios, O_RDONLY);
	if (!bios || !core->loadBIOS(core, bios, 0)) {
		printf("FAIL: bios rejected (%s)\n", g_bios);
		return NULL;
	}
	if (!mCoreLoadFile(core, g_rom)) {
		printf("FAIL: rom rejected (%s)\n", g_rom);
		return NULL;
	}
	mCoreConfigInit(&core->config, "netrunner-test");
	mCoreConfigMap(&core->config, &opts);
	opts.audioSync = false;
	opts.videoSync = false;
	mCoreConfigLoadDefaults(&core->config, &opts);
	mCoreLoadConfig(core);

	/* The wall clock is a hidden input: pin the RTC. */
	core->rtc.override = RTC_FAKE_EPOCH;
	core->rtc.value = 1200000000000LL / 1000;
	core->reset(core);
	return core;
}

static void _multiSetup(struct GBASIONetRunner* runner) {
	int i;
	struct GBA* g;
	for (i = 0; i < 2; ++i) {
		g = runner->cores[i].core->board;
		g->sio.p->memory.io[GBA_REG(SIOMLT_SEND)] = 0x0100 + i;
		/* MULTI mode (bits 12-13), baud 1, own id in bits 4-5. */
		GBASIOWriteSIOCNT(&g->sio, 0x2000 | (i << 4) | 0x0001);
	}
	for (i = 0; i < 2; ++i) {
		GBASIONetRunnerStep(runner, 0);
	}
	g = runner->cores[0].core->board;
	GBASIOWriteSIOCNT(&g->sio, 0x2000 | 0x0001 | 0x0080);
	for (i = 0; i < 5; ++i) {
		GBASIONetRunnerStep(runner, 0);
	}
}

/* Phase A: no session, so every core gets the same mask. */
static bool _runLocal(uint32_t out[GGA_NET_MAX_PLAYERS]) {
	struct GBASIONetRunner runner;
	uint32_t fc0, fc1;
	int i;

	GBASIONetRunnerInit(&runner, 2, NULL);
	CHECK(GBASIONetRunnerAddCore(&runner, 0, _loadCore()));
	CHECK(GBASIONetRunnerAddCore(&runner, 1, _loadCore()));

	_multiSetup(&runner);
	CHECK(runner.coord.transferMode == GBA_SIO_MULTI);

	for (i = 0; i < LOCAL_FRAMES; ++i) {
		CHECK(GBASIONetRunnerStep(&runner, (uint16_t) (0x10 + (i % 3))));
	}
	fc0 = runner.cores[0].core->frameCounter(runner.cores[0].core);
	fc1 = runner.cores[1].core->frameCounter(runner.cores[1].core);
	/* The frame group keeps both cores level; the parked-peer fallback can
	 * cost one of them an extra frame, so allow a tiny drift. */
	CHECK(fc0 >= LOCAL_FRAMES && fc1 >= LOCAL_FRAMES);
	CHECK((fc0 > fc1 ? fc0 - fc1 : fc1 - fc0) <= 2);
	GBASIONetRunnerHashes(&runner, out);
	GBASIONetRunnerDeinit(&runner);
	runner.cores[0].core->deinit(runner.cores[0].core);
	runner.cores[1].core->deinit(runner.cores[1].core);
	return true;
}

static bool _testLocal(void) {
	uint32_t first[GGA_NET_MAX_PLAYERS] = { 0 };
	uint32_t second[GGA_NET_MAX_PLAYERS] = { 0 };

	if (!_runLocal(first) || !_runLocal(second)) {
		return false;
	}
	CHECK(memcmp(first, second, sizeof(first)) == 0);
	printf("  local: %d frames lockstep, MULTI ok, hashes %08x/%08x twice\n",
		LOCAL_FRAMES, first[0], first[1]);
	return true;
}

static bool _queueHandshake(struct GBASIONetEndpoint* he, struct GBASIONetEndpoint* ge, int port) {
	GBASIOLockstepCoordinatorInit(&g_hc);
	GBASIOLockstepDriverCreate(&g_hd, 0);
	GBASIONetLinkInit(&g_hl, &g_hc, &g_hd, 0);
	GBASIONetEndpointInit(he, &g_hl, &g_transport);
	if (!GBASIONetEndpointListen(he, port)) {
		printf("FAIL: listen on %d\n", port);
		return false;
	}
	GBASIOLockstepCoordinatorInit(&g_gc);
	GBASIOLockstepDriverCreate(&g_gd, 0);
	GBASIONetLinkInit(&g_gl, &g_gc, &g_gd, 0);
	GBASIONetEndpointInit(ge, &g_gl, &g_transport);
	if (!GBASIONetEndpointConnect(ge, "127.0.0.1", port)) {
		printf("FAIL: connect to %d\n", port);
		return false;
	}
	return true;
}

/* Phase B: two clients, two cores each, masks only on the wire. */
static bool _testNet(void) {
	struct GBASIONetEndpoint he, ge;
	struct GBASIONetSession hs, gs;
	struct GBASIONetRunner hr, gr;
	int compared = 0;
	int stuck = 0;
	int i;

	GBASIONetRunnerInit(&hr, 2, &hs);
	GBASIONetRunnerInit(&gr, 2, &gs);
	CHECK(GBASIONetRunnerAddCore(&hr, 0, _loadCore()));
	CHECK(GBASIONetRunnerAddCore(&hr, 1, _loadCore()));
	CHECK(GBASIONetRunnerAddCore(&gr, 0, _loadCore()));
	CHECK(GBASIONetRunnerAddCore(&gr, 1, _loadCore()));
	if (!_queueHandshake(&he, &ge, PORT)) {
		return false;
	}

	GBASIONetSessionInit(&hs, GGA_NET_SESSION_HOST, 0, 2, _nowMs, 0);
	GBASIONetSessionInit(&gs, GGA_NET_SESSION_GUEST, 1, 2, _nowMs, 0);
	hs.inputDeadlineMs = 100;
	gs.inputDeadlineMs = 100;
	GBASIONetSessionSetPeer(&hs, &he);
	GBASIONetSessionSetPeer(&gs, &ge);
	GBASIONetSessionSetLocalInput(&gs, 0x0020);
	GBASIONetSessionStart(&gs);

	for (i = 0; i < RUN_FRAMES + 4 * GGA_NET_TICK_AHEAD + 50; ++i) {
		bool h, g;
		if (GBASIONetSessionEnded(&hs) || GBASIONetSessionEnded(&gs)) {
			break;
		}
		++g_now;
		GBASIONetEndpointPoll(&ge);
		GBASIONetEndpointPoll(&he);
		GBASIONetSessionSetLocalInput(&hs, (GBASIONetSessionFrame(&hs) & 1) ? 0x0010 : 0);
		GBASIONetSessionSetLocalInput(&gs, (uint16_t) (0x0020 | (GBASIONetSessionFrame(&gs) & 0x10)));
		h = GBASIONetRunnerStep(&hr, 0);
		g = GBASIONetRunnerStep(&gr, 0);
		if (!h && !g) {
			if (++stuck > 400) {
				printf("  net: stalled at host %llu guest %llu\n",
					(unsigned long long) hr.frames, (unsigned long long) gr.frames);
				break;
			}
		} else {
			stuck = 0;
		}
	}

	CHECK(!GBASIONetSessionEnded(&hs));
	CHECK(!GBASIONetSessionEnded(&gs));
	CHECK(!hs.desync && !gs.desync);
	CHECK(hr.frames >= RUN_FRAMES);
	CHECK(gr.frames >= RUN_FRAMES);
	CHECK(hr.frames >= gr.frames);
	CHECK(hr.frames - gr.frames <= GGA_NET_TICK_AHEAD + 4);
	for (i = 0; i < GGA_NET_HASH_WINDOW; ++i) {
		if (hs.hashMineValid[i] && hs.hashTheirsValid[i]) {
			++compared;
		}
	}
	CHECK(compared >= 2);

	GBASIONetEndpointClose(&ge);
	GBASIONetEndpointClose(&he);
	GBASIOLockstepCoordinatorDeinit(&g_hc);
	GBASIOLockstepCoordinatorDeinit(&g_gc);
	GBASIONetRunnerDeinit(&hr);
	GBASIONetRunnerDeinit(&gr);
	printf("  net: %llu frames over the wire, %d real hashes matched clean\n",
		(unsigned long long) hr.frames, compared);
	return true;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	mLogSetDefaultLogger(&_quietLogger);
	if (argc < 3) {
		printf("usage: %s ROM BIOS\n", argv[0]);
		return 2;
	}
	g_rom = argv[1];
	g_bios = argv[2];

	if (!_testLocal()) {
		return 1;
	}
	if (!_testNet()) {
		return 1;
	}
	printf("NETRUNNER PASS\n");
	return 0;
}
