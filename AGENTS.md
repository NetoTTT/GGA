# GGA — Agent Handbook

> Entry point for humans and AI agents. Read this before changing the
> repo. **Project language is English** (code, comments, docs, commits).
> Details live in `docs/`; the current netplay direction is
> `docs/NETPLAY-ARCHITECTURE.md`.

## 1. What this is

**GGA (Game Girl Advanced)**: mobile-first GBA emulator with 4-player
online link play and PC↔mobile crossplay. Public, open source.

- Emulation core imported from [mGBA](https://github.com/mgba-emu/mgba)
  (full history). Deterministic 4-player link already exists upstream as
  `src/gba/sio/lockstep.c` (coordinator + players 0-3).
- GGA's own work: netplay transport, the input-sync netplay model
  (see `docs/NETPLAY-ARCHITECTURE.md`), a future Android frontend, and a
  lobby hub.

## 2. Repo map

| Path | Purpose |
| --- | --- |
| `src/gba/`, `include/mgba/` | mGBA core (MPL-2.0, derived — headers intact) |
| `src/gba/sio/lockstep.c` | local deterministic link coordinator/driver |
| `src/gba/sio/net.c`, `netep.c`, `netdriver.c` | GGA netplay transport + bridge |
| `include/mgba/internal/gba/sio/net*.h` | netplay internal types + wire IDs |
| `include/mgba-util/socket.h` | portable TCP/udp sockets (incl. Switch) |
| `src/platform/qt/` | Qt frontend (LAN tab: Host/Join, port 27182) |
| `gga/` | netplay harnesses/tools (e.g. `net-selftest.c`, `link-netharness2.c`) |
| `docs/` | `NETPLAY-ARCHITECTURE.md` (decision), `NETPLAY.md` (v1), `ROADMAP.md` |
| `.github/workflows/` | CI (Linux gcc/SDL, Windows MSYS2 MinGW/SDL) |

## 3. Conventions

### Code
- Core in **C99**, Qt frontend in **C++**. Tabs, `.clang-format` at root.
- English identifiers and comments. **Do not add comments unless asked.**
- In-tree files (`src/`, `include/`) keep the **MPL-2.0 header** — never
  strip or rewrite license headers.

### Commits
- Short subsystem prefix (`net:`, `sio:`, `docs:` …), imperative, ≤72
  chars subject. One concern per commit.
- Repo is **LF**. Files edited on Windows may become CRLF — normalize to
  LF before committing.

### Diagnostics (temporary instrumentation)
- Keep always-on logs out of hot paths. Mark temporary debug lines with a
  stable tag so they are greppable and revertible:
  - core logs: `DBG <name> ...`
  - wire logs: `NET tx|rx id=<n> len=<n>`
- **Record every temporary diagnostic** you add in the commit/PR notes so
  it can be removed after validation. Throttle high-frequency logs.

### Protocol & identifiers
- Source of truth for wire IDs is the header, not prose: see
  `include/mgba/internal/gba/sio/net*.h` (`GGA_NETW_*`).
- SIO mode enum lives in `include/mgba/gba/interface.h`
  (`GBA_SIO_NORMAL_8=0`, `NORMAL_32=1`, `MULTI=2`, `UART=3`, `GPIO=8`,
  `JOYBUS=12`).

### Research & decisions
- Record non-trivial architecture decisions as an English doc under
  `docs/` (see `NETPLAY-ARCHITECTURE.md`) with Status, Context,
  Evidence, Decision, and References. Cite sources.

### Status block format
When reporting or documenting state, use this exact shape so it stays
machine-parseable:

```
Status: <done | in progress | blocked>
Done: <completed, verified items>
In progress: <single active item>
Blocked: <blocker + why>
Next: <one concrete next step>
```

## 4. Build & verify

Fast dev loop (Linux/WSL, SDL):

```bash
sudo apt-get install -y build-essential cmake libsdl2-dev \
  zlib1g-dev libpng-dev libzip-dev
cmake -S . -B build -DBUILD_SDL=ON -DBUILD_QT=OFF \
  -DUSE_FFMPEG=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"        # -> build/sdl/mgba
gcc -I include gga/net-selftest.c src/gba/sio/net.c \
  -o /tmp/net-selftest -pthread && /tmp/net-selftest
```

Windows native route: MSVC Build Tools + Qt via `aqtinstall` + Ninja,
with a **minimal `PATH`** (MSYS2/Strawberry entries leak MinGW headers
into `cl.exe`). Known gotchas:

- MinGW GCC 16 pseudo-reloc overflow on the Qt exe — use the MSVC route,
  not `-mcmodel` hacks.
- vcpkg zlib/libpng break on new MSVC — build them from source into a
  local prefix instead.
- Never `rm -rf` a deploy dir without re-running
  `windeployqt --compiler-runtime` plus the runtime DLL set afterwards.

macOS is **not** a target (no maintainer hardware — do not add macOS
CI/jobs).

## 5. Netplay status

Netplay is pivoting to **deterministic input netplay** (all linked cores
local, network carries inputs + hashes). Read
`docs/NETPLAY-ARCHITECTURE.md` before touching netplay code. `docs/NETPLAY.md`
documents the superseded v1 model and the transport/handshake still in use.

## 6. CI

- `build.yml`: **Linux** (gcc, SDL) + **Windows** (MSYS2 MinGW, SDL).
  Both must stay green. No macOS job by policy.
- Keep CI fast: SDL-only, Release, no FFmpeg. Qt coverage is upstream's
  job, not ours.

## 7. License rules for agents

- Files derived from mGBA: MPL-2.0, headers intact, `LICENSE` untouched.
- New standalone files outside `src/`/`include/`: MIT default.
- When in doubt between convenience and license safety, choose safety.
- **Never commit**: binaries, ROMs, BIOS dumps, credentials, signing keys,
  user data, or build dirs (`*-build/`, `dist/`).

## 8. Glossary

- **SIO**: GBA serial I/O (link cable). Registers: `SIOCNT`, `RCNT`,
  `SIOMLT_SEND`, `SIOMULTI0-3`.
- **MULTI**: 16-bit multi-player SIO mode (up to 4 GBAs); the mode used
  by almost all link games.
- **Lockstep**: deterministic multi-core link where each transfer is a
  synchronized barrier across all attached cores.
- **v1 / v2**: transport-forwarded SIO (v1, superseded) vs input netplay
  (v2, target).
