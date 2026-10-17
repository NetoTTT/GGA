/* v2 startup blob exchange over the netep endpoint.
 *
 * No emulated core: it drives a host endpoint and a client endpoint over
 * TCP loopback and checks that, right after WELCOME, the host ships the
 * deterministic RTC seed plus the ROM and save blobs and that the client
 * reassembles and hash-verifies them through the real endpoint code.
 *
 * Build (WSL, after rebuilding libmgba):
 *   gcc -I include gga/ep-blob-test.c -L$HOME/gga-build -lmgba -lpthread \
 *     -lm -ldl -o $HOME/ep-blob-test
 */
#include <mgba/internal/gba/sio/netep.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_PORT 27196
#define ROM_LEN 70000
#define SAVE_LEN 3000
#define SEED 1200000000000LL

static uint8_t s_hash[32];
static struct GBASIONetEndpoint g_host;
static struct GBASIONetEndpoint g_client;

static uint8_t _pat(uint8_t kind, size_t i) {
	return (uint8_t) (i * (kind == GGA_NET_BLOB_ROM ? 13 : 7) + kind);
}

static bool _getBlob(void* context, uint8_t kind, void** bytes, size_t* size) {
	size_t n = kind == GGA_NET_BLOB_ROM ? ROM_LEN : SAVE_LEN;
	uint8_t* b = malloc(n);
	size_t i;
	(void) context;
	if (!b) {
		return false;
	}
	for (i = 0; i < n; ++i) {
		b[i] = _pat(kind, i);
	}
	*bytes = b;
	*size = n;
	return true;
}

static void _getRtcSeed(void* context, int64_t* epochMs) {
	(void) context;
	*epochMs = SEED;
}

static bool s_gotRom, s_gotSave, s_romOk, s_saveOk, s_seedOk;

static void _loadBlob(void* context, uint8_t kind, const void* bytes, size_t size) {
	size_t n = kind == GGA_NET_BLOB_ROM ? ROM_LEN : SAVE_LEN;
	const uint8_t* b = bytes;
	size_t i;
	bool ok = size == n;
	(void) context;
	if (ok) {
		for (i = 0; i < n; ++i) {
			if (b[i] != _pat(kind, i)) {
				ok = false;
				break;
			}
		}
	}
	if (kind == GGA_NET_BLOB_ROM) {
		s_gotRom = true;
		s_romOk = ok;
	} else {
		s_gotSave = true;
		s_saveOk = ok;
	}
}

static void _loadRtcSeed(void* context, int64_t epochMs) {
	(void) context;
	s_seedOk = epochMs == SEED;
}

int main(void) {
	static struct GBASIOLockstepCoordinator hc, cc;
	static struct GBASIOLockstepDriver hd, cd;
	static struct GBASIONetLink hl, cl;
	struct GBASIONetTransport hostT;
	struct GBASIONetTransport clientT;
	int i;

	memset(s_hash, 0xCD, sizeof(s_hash));
	memset(&hostT, 0, sizeof(hostT));
	hostT.getBlob = _getBlob;
	hostT.getRtcSeed = _getRtcSeed;
	memset(&clientT, 0, sizeof(clientT));
	clientT.loadBlob = _loadBlob;
	clientT.loadRtcSeed = _loadRtcSeed;

	GBASIOLockstepCoordinatorInit(&hc);
	GBASIOLockstepDriverCreate(&hd, 0);
	GBASIONetLinkInit(&hl, &hc, &hd, 0);
	GBASIONetEndpointInit(&g_host, &hl, &hostT);
	GBASIONetEndpointSetIdentity(&g_host, s_hash, "host");
	if (!GBASIONetEndpointListen(&g_host, TEST_PORT)) {
		printf("FAIL: listen\n");
		return 1;
	}

	GBASIOLockstepCoordinatorInit(&cc);
	GBASIOLockstepDriverCreate(&cd, 0);
	GBASIONetLinkInit(&cl, &cc, &cd, 0);
	GBASIONetEndpointInit(&g_client, &cl, &clientT);
	GBASIONetEndpointSetIdentity(&g_client, s_hash, "client");
	if (!GBASIONetEndpointConnect(&g_client, "127.0.0.1", TEST_PORT)) {
		printf("FAIL: connect\n");
		return 1;
	}

	for (i = 0; i < 3000 && !(s_gotRom && s_gotSave && s_seedOk); ++i) {
		GBASIONetEndpointPoll(&g_host);
		GBASIONetEndpointPoll(&g_client);
		usleep(1000);
	}

	if (!s_gotRom || !s_gotSave || !s_romOk || !s_saveOk || !s_seedOk) {
		printf("FAIL: rom=%d save=%d romOk=%d saveOk=%d seedOk=%d\n",
			s_gotRom, s_gotSave, s_romOk, s_saveOk, s_seedOk);
		fflush(stdout);
		return 1;
	}
	printf("EP-BLOB PASS\n");
	fflush(stdout);

	GBASIONetEndpointClose(&g_client);
	GBASIONetEndpointClose(&g_host);
	GBASIONetLinkDeinit(&hl);
	GBASIONetLinkDeinit(&cl);
	GBASIOLockstepCoordinatorDeinit(&hc);
	GBASIOLockstepCoordinatorDeinit(&cc);
	SocketSubsystemDeinit();
	printf("teardown ok\n");
	return 0;
}
