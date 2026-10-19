/* Endpoint accept smoke test: listen, poll, report. No core, no threads. */
#include <mgba/internal/gba/sio/netep.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
	static struct GBASIOLockstepCoordinator coord;
	static struct GBASIOLockstepDriver driver;
	static struct GBASIONetLink link;
	static struct GBASIONetEndpoint ep;
	uint8_t hash[32];
	memset(hash, 0xAB, sizeof(hash));
	GBASIOLockstepCoordinatorInit(&coord);
	GBASIOLockstepDriverCreate(&driver, 0);
	GBASIONetLinkInit(&link, &coord, &driver, 0);
	GBASIONetEndpointInit(&ep, &link, 0);
	GBASIONetEndpointSetIdentity(&ep, hash, "smoke");
	if (!GBASIONetEndpointListen(&ep, 27182)) {
		printf("FAIL: listen\n");
		return 1;
	}
	printf("listening, Doble check with: python3 -c \"import socket; "
	       "c=socket.create_connection(('127.0.0.1',27182)); print('client ok')\"\n");
	fflush(stdout);
	printf("ep=%p link=%p\n", (void*) &ep, (void*) ep.link);
	fprintf(stderr, "ep=%p link=%p\n", (void*) &ep, (void*) ep.link);
	for (int i = 0; i < 200; ++i) {
		GBASIONetEndpointPoll(&ep);
		if (!SOCKET_FAILED(ep.conn)) {
			printf("ACCEPTED fd=%d\n", (int) ep.conn);
			break;
		}
		usleep(50000);
	}
	GBASIONetEndpointClose(&ep);
	printf("done\n");
	return 0;
}
