# GGA — Game Girl Advanced

<p align="center">
  <img src="docs/assets/gga-icon.png" alt="GGA icon" width="256" />
</p>

A mobile-first Game Boy Advance emulator with **4-player online link play**,
built for phones and PCs to play together.

> Status: mGBA-derived core with GGA LAN netplay work in progress. The
> current netplay direction is deterministic input sync; see
> [docs/NETPLAY-ARCHITECTURE.md](docs/NETPLAY-ARCHITECTURE.md) and
> [docs/ROADMAP.md](docs/ROADMAP.md).

## Vision

- **Mobile-first**: touch controls, battery-friendly, Android from day one.
- **4-player link play**: real multiplayer for the dozens of GBA games that
  support it — over LAN first, internet later.
- **Lobby hub**: discover rooms, join by code, no account needed on LAN.
- **PC + mobile crossplay**: same protocol, same core, every screen.

## Project layout

- `src/`, `include/`, `res/`, `tools/` — emulation core and existing
  frontends, imported from mGBA (see Credits).
- `gga/` — GGA netplay harnesses and command-line tools.
- `docs/` — design docs and roadmap.

## Building

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) for the CLI build and LAN test
workflow. The Qt LAN v2 implementation is under active development.

## Credits

GGA stands on the shoulders of **[mGBA](https://github.com/mgba-emu/mgba)**
by endrift and contributors — an accurate, open GBA core with a 4-player
lockstep link implementation that this project extends to online play.
Thank you.

Upstream: `git remote add upstream https://github.com/mgba-emu/mgba.git`
is preconfigured in clones made from this repo; syncs are welcome.

## License

- Files derived from mGBA remain under the
  [Mozilla Public License 2.0](LICENSE), with their headers intact.
- New GGA-authored files (under `gga/` and `docs/`) are MIT licensed
  unless stated otherwise (proposed default — open an issue to discuss).
