/* v2 input-sync session: host tick authority, guest buffering, backpressure.
 *
 * No emulated cores. Two sessions (host + guest) talk over TCP loopback
 * while a synthetic clock and a trivial "frame" stand in for the emulation
 * loop, so the tick ordering can be checked exactly.
 *
 * Build (WSL, after rebuilding libmgba):
 *   gcc -I include gga/netsession-test.c -L$HOME/gga-build -lmgba -lpthread \
 *     -lm -ldl -o $HOME/netsession-test
 */
#include <mgba/internal/gba/sio/netsession.h>

#include <mgba/internal/gba/sio/netep.h>
#include <mgba/internal/gba/sio/netdriver.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LIMIT 61
#define PORT_A 27195
#define PORT_B 27194
#define HOST_KEY(f) ((uint16_t) (0x100 + (f)))
#define GUEST_KEY(f) ((uint16_t) (0x200 + (f)))

static int64_t g_now;

static struct GBASIONetEndpoint g_he, g_ge;
static struct GBASIONetSession g_hs, g_gs;
static struct GBASIOLockstepCoordinator g_hc, g_gc;
static struct GBASIOLockstepDriver g_hd, g_gd;
static struct GBASIONetLink g_hl, g_gl;

/* Recorded by the driver loop. */
static uint16_t g_hostGuestKeys[LIMIT];  /* keys the host used for the guest */
static uint16_t g_hostOwnKeys[LIMIT];    /* keys the host used for itself */
static uint16_t g_guestHostKeys[LIMIT];  /* host keys as the guest applied them */
static uint16_t g_announced[LIMIT];      /* guest announcement per frame */

static int64_t _nowMs(void* context) {
	(void) context;
	return g_now;
}

static int _setup(struct GBASIONetEndpoint* he, struct GBASIONetEndpoint* ge, int port) {
	static uint8_t hash[32];
	static struct GBASIONetTransport transport;
	memset(he, 0, sizeof(*he));
	memset(ge, 0, sizeof(*ge));
	memset(&transport, 0, sizeof(transport));

	GBASIOLockstepCoordinatorInit(&g_hc);
	GBASIOLockstepDriverCreate(&g_hd, 0);
	GBASIONetLinkInit(&g_hl, &g_hc, &g_hd, 0);
	GBASIONetEndpointInit(he, &g_hl, &transport);
	GBASIONetEndpointSetIdentity(he, hash, "host");
	if (!GBASIONetEndpointListen(he, port)) {
		printf("FAIL: listen\n");
		return 1;
	}
	GBASIOLockstepCoordinatorInit(&g_gc);
	GBASIOLockstepDriverCreate(&g_gd, 0);
	GBASIONetLinkInit(&g_gl, &g_gc, &g_gd, 0);
	GBASIONetEndpointInit(ge, &g_gl, &transport);
	GBASIONetEndpointSetIdentity(ge, hash, "guest");
	if (!GBASIONetEndpointConnect(ge, "127.0.0.1", port)) {
		printf("FAIL: connect\n");
		return 1;
	}
	return 0;
}

static void _teardown(void) {
	GBASIONetEndpointClose(&g_ge);
	GBASIONetEndpointClose(&g_he);
	GBASIONetLinkDeinit(&g_hl);
	GBASIONetLinkDeinit(&g_gl);
	GBASIOLockstepCoordinatorDeinit(&g_hc);
	GBASIOLockstepCoordinatorDeinit(&g_gc);
}

static bool _fail(const char* what, int line) {
	printf("FAIL line %d: %s\n", line, what);
	return false;
}
#define CHECK(c) do { if (!(c)) { return _fail(#c, __LINE__); } } while (0)

/* Phase A: both sides run 60 frames; every key mask must land on the frame
 * the protocol says it should, and the host must never fall back to idle. */
static bool _testPipeline(void) {
	uint16_t keys[GGA_NET_MAX_PLAYERS];
	int i;

	if (_setup(&g_he, &g_ge, PORT_A)) {
		return false;
	}
	GBASIONetSessionInit(&g_hs, GGA_NET_SESSION_HOST, 0, 2, _nowMs, 0);
	GBASIONetSessionInit(&g_gs, GGA_NET_SESSION_GUEST, 1, 2, _nowMs, 0);
	g_hs.inputDeadlineMs = 5;
	g_gs.inputDeadlineMs = 5;
	GBASIONetSessionSetPeer(&g_hs, &g_he);
	GBASIONetSessionSetPeer(&g_gs, &g_ge);
	GBASIONetSessionSetLocalInput(&g_gs, GUEST_KEY(0));
	GBASIONetSessionStart(&g_gs);

	for (i = 0; i < 5000; ++i) {
		uint32_t hf = GBASIONetSessionFrame(&g_hs);
		uint32_t gf = GBASIONetSessionFrame(&g_gs);
		if (hf >= LIMIT - 1 && gf >= LIMIT - 1) {
			break;
		}
		++g_now;
		GBASIONetEndpointPoll(&g_ge);
		GBASIONetEndpointPoll(&g_he);

		if (hf < LIMIT) {
			GBASIONetSessionSetLocalInput(&g_hs, HOST_KEY(hf));
			if (GBASIONetSessionNextFrame(&g_hs, keys)) {
				g_hostOwnKeys[hf] = keys[0];
				g_hostGuestKeys[hf] = keys[1];
			}
		}
		if (gf < LIMIT) {
			GBASIONetSessionSetLocalInput(&g_gs, GUEST_KEY(gf));
			if (GBASIONetSessionNextFrame(&g_gs, keys)) {
				g_guestHostKeys[gf] = keys[0];
				/* The guest announced our keys for frame gf + DELAY. */
				if (gf + GGA_NET_INPUT_DELAY < LIMIT) {
					g_announced[gf + GGA_NET_INPUT_DELAY] = GUEST_KEY(gf);
				}
			}
		}
	}

	CHECK(GBASIONetSessionFrame(&g_hs) >= LIMIT - 1);
	CHECK(GBASIONetSessionFrame(&g_gs) >= LIMIT - 1);
	CHECK(!g_hs.failed && !g_gs.failed);
	CHECK(!g_hs.lagging); /* no idle fill once past the startup window */

	for (i = 0; i < LIMIT - 1; ++i) {
		/* Host's own keys are sampled at run time. */
		CHECK(g_hostOwnKeys[i] == HOST_KEY(i));
		/* Guest keys: frames before the first answer are the startup
		 * announcement, the rest come back DELAY frames later. */
		uint16_t want = i < GGA_NET_INPUT_DELAY ? GUEST_KEY(0) : GUEST_KEY(i - GGA_NET_INPUT_DELAY);
		if (g_hostGuestKeys[i] != want) {
			printf("FAIL: host frame %d guest keys %04x want %04x\n",
				i, g_hostGuestKeys[i], want);
			return false;
		}
		/* The guest applied the host's authoritative tick verbatim. */
		CHECK(g_guestHostKeys[i] == HOST_KEY(i));
	}
	_teardown();
	printf("  pipeline: 60 frames, keys aligned, no idle fill\n");
	return true;
}

/* Phase B: the guest never steps. The host may coast on the idle mask for
 * the ahead window, then must stop instead of running away from it. */
static bool _testBackpressure(void) {
	uint16_t keys[GGA_NET_MAX_PLAYERS];
	int i;
	uint32_t stuck = 0;

	if (_setup(&g_he, &g_ge, PORT_B)) {
		return false;
	}
	GBASIONetSessionInit(&g_hs, GGA_NET_SESSION_HOST, 0, 2, _nowMs, 0);
	GBASIONetSessionInit(&g_gs, GGA_NET_SESSION_GUEST, 1, 2, _nowMs, 0);
	g_hs.inputDeadlineMs = 2;
	g_gs.inputDeadlineMs = 2;
	GBASIONetSessionSetPeer(&g_hs, &g_he);
	GBASIONetSessionSetPeer(&g_gs, &g_ge);
	GBASIONetSessionSetLocalInput(&g_gs, GUEST_KEY(0));
	GBASIONetSessionStart(&g_gs);

	for (i = 0; i < 5000; ++i) {
		uint32_t hf = GBASIONetSessionFrame(&g_hs);
		++g_now;
		GBASIONetEndpointPoll(&g_ge);
		GBASIONetEndpointPoll(&g_he);
		GBASIONetSessionSetLocalInput(&g_hs, HOST_KEY(hf));
		if (GBASIONetSessionNextFrame(&g_hs, keys)) {
			continue;
		}
		if (GBASIONetSessionFrame(&g_hs) == hf && hf > 0) {
			if (++stuck > 200) {
				break; /* stopped for good */
			}
		}
	}

	CHECK(!g_hs.failed);
	CHECK(g_hs.lagging); /* it had to fill idle to get this far */
	/* The guest announced DELAY frames at start, so it is known to have run
	 * none: the host can cover frames 0..AHEAD only. */
	if (GBASIONetSessionFrame(&g_hs) != GGA_NET_TICK_AHEAD + 1) {
		printf("FAIL: host stopped at frame %u want %u\n",
			GBASIONetSessionFrame(&g_hs), GGA_NET_TICK_AHEAD + 1);
		return false;
	}
	_teardown();
	printf("  backpressure: host capped at frame %u with the guest stalled\n",
		GGA_NET_TICK_AHEAD + 1);
	return true;
}

int main(void) {
	if (!_testPipeline()) {
		_teardown();
		return 1;
	}
	if (!_testBackpressure()) {
		_teardown();
		return 1;
	}
	SocketSubsystemDeinit();
	printf("NETSESSION PASS\n");
	return 0;
}
