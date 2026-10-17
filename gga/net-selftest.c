/* GGA net transport loopback self-test.
 *
 * Build (Linux, no CMake needed — net.c is dependency-free):
 *   gcc -I include gga/net-selftest.c src/gba/sio/net.c -o /tmp/net-selftest -pthread
 *   /tmp/net-selftest
 *
 * Exercises: listen/connect, HELLO -> WELCOME, TRANSFER echo,
 * 64 KiB STATE blob round-trip, ROM/save blob round-trip, RTC seed,
 * REJECT on bad proto, heartbeat.
 */
#include <mgba/internal/gba/sio/net.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_PORT 27199
#define CHECK(c) do { \
		if (!(c)) { \
			printf("FAIL line %d: %s\n", __LINE__, #c); \
			return 1; \
		} \
	} while (0)

static uint8_t s_romHash[32];
/* 130001 = 60000 + 60000 + 10001, so it crosses two chunk boundaries. */
static uint8_t s_romBlob[130001];
static size_t s_romBlobLen = sizeof(s_romBlob);
static int64_t s_rtcSeed = 1200000000000LL;

static void* _server(void* unused) {
	(void) unused;
	Socket listenSock = SocketOpenTCP(TEST_PORT, 0);
	if (SOCKET_FAILED(listenSock)) {
		printf("FAIL: listen\n");
		return (void*) 1;
	}
	SocketListen(listenSock, 1);
	for (int session = 0; session < 2; ++session) {
	Socket conn = SocketAccept(listenSock, 0);
	if (SOCKET_FAILED(conn)) {
		printf("FAIL: accept\n");
		return (void*) 1;
	}
	uint8_t id;
	uint8_t payload[GGA_NET_MAX_PAYLOAD];
	uint16_t len;
	/* HELLO -> WELCOME or REJECT */
	if (!GGANetRecvFrame(conn, &id, payload, &len, 5000) || id != GGA_NET_HELLO) {
		printf("FAIL: hello\n");
		return (void*) 1;
	}
	if (payload[0] != GGA_NET_PROTO) {
		GGANetSendReject(conn, GGA_NET_REJECT_PROTO);
	} else if (GGANetLoadU32(&payload[1]) != GGA_NET_CORE_VERSION) {
		GGANetSendReject(conn, GGA_NET_REJECT_CORE);
	} else if (memcmp(&payload[6], s_romHash, 32) != 0) {
		GGANetSendReject(conn, GGA_NET_REJECT_ROM);
	} else {
		GGANetSendWelcome(conn, 2, 1234);
		/* v2 startup: receive the ROM blob, answer with a save blob
		 * and the deterministic RTC seed. */
		uint8_t kind = 0xFF;
		void* blob = 0;
		size_t blobLen = 0;
		if (!GGANetRecvBlob(conn, &kind, &blob, &blobLen, 10000)) {
			printf("FAIL: recv rom blob\n");
			return (void*) 1;
		}
		if (kind != GGA_NET_BLOB_ROM || blobLen != s_romBlobLen ||
			memcmp(blob, s_romBlob, blobLen) != 0) {
			printf("FAIL: rom blob mismatch kind=%u len=%zu\n", kind, blobLen);
			return (void*) 1;
		}
		free(blob);
		static uint8_t saveBlob[5000];
		for (size_t i = 0; i < sizeof(saveBlob); ++i) {
			saveBlob[i] = (uint8_t) (i * 7 + 3);
		}
		if (!GGANetSendBlob(conn, GGA_NET_BLOB_SAVE, saveBlob, sizeof(saveBlob)) ||
			!GGANetSendRtcSeed(conn, s_rtcSeed)) {
			printf("FAIL: send save/rtc\n");
			return (void*) 1;
		}
	}
	/* Echo TRANSFERs, answer STATE_REQ with a blob, then quit on BYE. */
	for (;;) {
		if (!GGANetRecvFrame(conn, &id, payload, &len, 5000)) {
			break;
		}
		if (id == GGA_NET_TRANSFER_START) {
			GGANetSendFrame(conn, id, payload, len);
		} else if (id == GGA_NET_STATE_REQ) {
			static uint8_t blob[60000];
			for (size_t i = 0; i < sizeof(blob); ++i) {
				blob[i] = (uint8_t) (i * 31 + 7);
			}
			GGANetSendFrame(conn, GGA_NET_STATE, blob, sizeof(blob));
		} else if (id == GGA_NET_BYE) {
			SocketClose(conn);
			break;
		}
		/* HEARTBEAT and anything else: ignore, keep serving. */
	}
	}
	SocketClose(listenSock);
	return 0;
}

int main(void) {
	memset(s_romHash, 0xAB, sizeof(s_romHash));
	for (size_t i = 0; i < s_romBlobLen; ++i) {
		s_romBlob[i] = (uint8_t) (i * 13 + 5);
	}
	SocketSubsystemInit();
	pthread_t server;
	pthread_create(&server, 0, _server, 0);

	struct Address addr;
	addr.version = IPV4;
	addr.ipv4 = 0x7F000001;
	Socket conn = INVALID_SOCKET;
	for (int attempt = 0; attempt < 25 && SOCKET_FAILED(conn); ++attempt) {
		conn = SocketConnectTCP(TEST_PORT, &addr);
		if (SOCKET_FAILED(conn)) {
			struct timespec quarter = { 0, 200 * 1000 * 1000 };
			nanosleep(&quarter, 0);
		}
	}
	CHECK(!SOCKET_FAILED(conn));
	SocketSetTCPPush(conn, 1);

	uint8_t id;
	static uint8_t payload[GGA_NET_MAX_PAYLOAD];
	uint16_t len;

	/* 1. bad proto -> REJECT */
	CHECK(GGANetSendHello(conn, 99, GGA_NET_CORE_VERSION, s_romHash, "t"));
	CHECK(GGANetRecvFrame(conn, &id, payload, &len, 5000));
	CHECK(id == GGA_NET_REJECT && len == 1 && payload[0] == GGA_NET_REJECT_PROTO);
	SocketClose(conn);

	/* 2. fresh connection, good HELLO -> WELCOME */
	conn = SocketConnectTCP(TEST_PORT, &addr);
	CHECK(!SOCKET_FAILED(conn));
	CHECK(GGANetSendHello(conn, GGA_NET_PROTO, GGA_NET_CORE_VERSION, s_romHash, "loopback"));
	CHECK(GGANetRecvFrame(conn, &id, payload, &len, 5000));
	CHECK(id == GGA_NET_WELCOME && len == 5 && payload[0] == 2);
	CHECK(GGANetLoadI32(&payload[1]) == 1234);

	/* 3. v2 startup blobs: ROM up, save + RTC seed down */
	{
		uint8_t kind = 0xFF;
		void* blob = 0;
		size_t blobLen = 0;
		int64_t rtc = 0;
		CHECK(GGANetSendBlob(conn, GGA_NET_BLOB_ROM, s_romBlob, s_romBlobLen));
		CHECK(GGANetRecvBlob(conn, &kind, &blob, &blobLen, 10000));
		CHECK(kind == GGA_NET_BLOB_SAVE && blobLen == 5000);
		for (size_t i = 0; i < blobLen; ++i) {
			if (((uint8_t*) blob)[i] != (uint8_t) (i * 7 + 3)) {
				printf("FAIL: save blob byte %zu\n", i);
				return 1;
			}
		}
		free(blob);
		CHECK(GGANetRecvRtcSeed(conn, &rtc, 5000));
		CHECK(rtc == s_rtcSeed);
	}

	/* 4. TRANSFER echo x50 */
	for (int i = 0; i < 50; ++i) {
		CHECK(GGANetSendTransfer(conn, 1000 + i, (uint8_t) (i & 3), (uint16_t) (0xBEEF ^ i)));
		CHECK(GGANetRecvFrame(conn, &id, payload, &len, 5000));
		CHECK(id == GGA_NET_TRANSFER_START && len == 7);
		CHECK(GGANetLoadI32(payload) == 1000 + i);
		CHECK(payload[4] == (uint8_t) (i & 3));
		CHECK(GGANetLoadU16(&payload[5]) == (uint16_t) (0xBEEF ^ i));
	}

	/* 5. STATE blob round-trip */
	CHECK(GGANetSendFrame(conn, GGA_NET_STATE_REQ, 0, 0));
	CHECK(GGANetRecvFrame(conn, &id, payload, &len, 10000));
	CHECK(id == GGA_NET_STATE && len == 60000);
	for (size_t i = 0; i < len; ++i) {
		if (payload[i] != (uint8_t) (i * 31 + 7)) {
			printf("FAIL: blob byte %zu\n", i);
			return 1;
		}
	}

	CHECK(GGANetSendHeartbeat(conn, 4242));
	CHECK(GGANetSendBye(conn, 0));
	SocketClose(conn);

	void* serverResult = 0;
	pthread_join(server, &serverResult);
	CHECK(serverResult == 0);
	SocketSubsystemDeinit();
	printf("NET-SELFTEST PASS\n");
	return 0;
}
