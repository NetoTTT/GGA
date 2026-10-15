# GGA — Agent Handbook

> Read this before working in this repo. Project language is English
> (code, docs, commits). This file is the entry point; details live in
> `docs/`.

## What this is

**GGA (Game Girl Advanced)**: mobile-first GBA emulator with 4-player
online link play and PC<->mobile crossplay. Public repo, open source.

- Emulation core: imported from [mGBA](https://github.com/mgba-emu/mgba)
  (full history included). Deterministic 4-player link already exists
  upstream as `src/gba/sio/lockstep.c` (coordinator + players 0-3).
- GGA's own work: network transport (`src/gba/sio/net.c`), lockstep
  proxy hooks (`AddProxy`/`ProxyEnqueue`), future Android frontend,
  lobby hub. See `docs/ROADMAP.md` and `docs/NETPLAY.md`.

## Current state (2026-10-14)

Done:

- Repo scaffold, README, roadmap, MPL-2.0 posture (derived files stay
  MPL; new standalone files MIT unless stated).
- Transport v1: framing, handshake, transfer, self-test
  (`gga/net-selftest.c` → `NET-SELFTEST PASS`).
- Lockstep proxy hooks compile into the SDL build (validated on
  x86_64 Linux, Ryzen, ~4 min full build).
- Wire protocol + 4P star-topology design in `docs/NETPLAY.md`.

Missing (in order):

1. Network lockstep driver (bridge transport <-> coordinator).
2. Android frontend (greenfield; SDL route planned).
3. Lobby hub (LAN discovery, rooms, join codes).
4. 4-device beta + crossplay validation.

## Code and structure rules

- Core in **C99**, Qt frontend in **C++**. Match surrounding style
  (tabs, `.clang-format` at root). English identifiers and comments.
- In-tree files (`src/`, `include/`) keep the **MPL-2.0 header** —
  never strip or rewrite license headers.
- Commit style: short subsystem prefix (`net:`, `sio:`, `docs:` …),
  imperative, ≤72 chars subject. One concern per commit.
- Never commit: binaries, ROMs, BIOS dumps, credentials, signing keys,
  user data, build dirs (`*-build/`, `dist/`).
- No game ROMs in repo or CI — contributors supply their own dumps for
  link tests. Desync asserts in lockstep are the test oracle.

## Build (fast dev loop, Linux/WSL)

```bash
sudo apt-get install -y build-essential cmake libsdl2-dev \
  zlib1g-dev libpng-dev libzip-dev
cmake -S . -B build -DBUILD_SDL=ON -DBUILD_QT=OFF \
  -DUSE_FFMPEG=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)          # → build/sdl/mgba
gcc -I include gga/net-selftest.c src/gba/sio/net.c \
  -o /tmp/net-selftest -pthread && /tmp/net-selftest
```

Windows native route: MSVC Build Tools + Qt via `aqtinstall` + Ninja;
minimal `PATH` (MSYS2/Strawberry entries leak MinGW headers into `cl.exe`).
Known gotchas: MinGW GCC 16 pseudo-reloc overflow on the Qt exe (use the
MSVC route, not `-mcmodel` hacks); vcpkg zlib/libpng break on new MSVC —
build them from source into a local prefix instead. Never `rm -rf` a
deploy dir without re-running `windeployqt --compiler-runtime` plus the
runtime DLL set afterwards. macOS is **not** a target (no maintainer
hardware — do not add macOS CI/jobs).

## CI (`.github/workflows/`)

- `build.yml`: **Linux** (gcc, SDL) + **Windows** (MSYS2 MinGW, SDL).
  Both must stay green. No macOS job by policy (above).
- Keep CI fast: SDL-only, Release, no FFmpeg. Qt coverage is upstream's
  job, not ours.

## License rules for agents

- Files derived from mGBA: MPL-2.0, headers intact, `LICENSE` untouched.
- New standalone files outside `src/`/`include/`: MIT default.
- When in doubt between convenience and license safety, choose safety
  and open an issue.
