/* Headless screenshot: init core, run N frames, dump raw framebuffer as PPM.
 * Build: gcc -DENABLE_VFS -DENABLE_DIRECTORIES -I include gga/shot.c
 *   -L/home/foque/gga-build -lmgba -lSDL2 -lEGL -lGL -lz -lpng16 -lzip
 *   -lpthread -lm -ldl -o /tmp/shot
 * Usage: shot ROM BIOS FRAMES OUT.PPM
 */
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
	if (argc < 5) {
		printf("usage: %s ROM BIOS FRAMES OUT.PPM\n", argv[0]);
		return 2;
	}
	int frames = atoi(argv[3]);
	struct mCore* core = mCoreFind(argv[1]);
	if (!core) {
		printf("FAIL: no core\n");
		return 1;
	}
	core->init(core);
	struct VFile* biosVf = VFileOpen(argv[2], O_RDONLY);
	if (!biosVf || !core->loadBIOS(core, biosVf, 0)) {
		printf("FAIL: bios\n");
		return 1;
	}
	if (!mCoreLoadFile(core, argv[1])) {
		printf("FAIL: rom\n");
		return 1;
	}
	mCoreConfigInit(&core->config, "shot");
	struct mCoreOptions opts = {};
	mCoreConfigMap(&core->config, &opts);
	opts.audioSync = false;
	opts.videoSync = false;
	mCoreConfigLoadDefaults(&core->config, &opts);
	mCoreLoadConfig(core);
	uint32_t* pixels = calloc(240 * 160, 4);
	core->setVideoBuffer(core, pixels, 240);
	core->reset(core);
	for (int i = 0; i < frames; ++i) {
		core->runFrame(core);
	}
	FILE* f = fopen(argv[4], "wb");
	fprintf(f, "P6\n240 160\n255\n");
	for (int i = 0; i < 240 * 160; ++i) {
		uint32_t p = pixels[i];
		fputc((p >> 16) & 0xFF, f);
		fputc((p >> 8) & 0xFF, f);
		fputc(p & 0xFF, f);
	}
	fclose(f);
	printf("SHOT frames=%d\n", frames);
	core->deinit(core);
	return 0;
}
