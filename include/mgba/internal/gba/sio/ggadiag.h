/* Copyright (c) 2026 NetoTTT
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_GGA_DIAG_H
#define GBA_SIO_GGA_DIAG_H

/* Netplay diagnostics are opt-in: set GGA_DIAG in the environment and the
 * per-transfer trace lines print. They sit in hot paths, so a normal run
 * must not pay for them; every site guards its fprintf (and any counter it
 * keeps) with GGA_DiagEnabled(). */
#include <stdlib.h>

static inline int GGA_DiagEnabled(void) {
	static int cached = -1;
	if (cached < 0) {
		const char* value = getenv("GGA_DIAG");
		cached = (value && value[0]) ? 1 : 0;
	}
	return cached;
}

#endif
