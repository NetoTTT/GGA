# Development build and LAN test

The Windows Qt build below is the configuration used for the GGA LAN frontend
and its scripted two-instance loopback test. It writes the runnable package to
`dist/`, which is ignored by Git.

## Windows Qt build

Install MSVC Build Tools, Qt 6 for MSVC, CMake, Ninja, vcpkg dependencies and
the local zlib prefix. The default paths match the maintainer workstation and
can be overridden with `GGA_VS_VCVARS`, `GGA_QT_ROOT`, `GGA_NINJA`,
`GGA_VCPKG_ROOT`, and `GGA_DEPS_ROOT`.

From a Developer PowerShell or Command Prompt:

```powershell
.\tools\build-qt-msvc.cmd
```

The script configures a Release Qt build with SDL, FFmpeg, PNG and libzip
disabled, builds the `GGA.exe` target and deploys its Qt runtime beside it in
`dist/`. Build intermediates stay under `build-qt-msvc/`.

## Two-instance LAN smoke test

Use a legally obtained GBA ROM that reaches gameplay. The script starts two
instances, hosts on loopback, joins the guest, and requires both instances to
reach the requested frame count while the session remains running. Per-process
logs are copied to `%TEMP%\gga-lan-autotest` by default.

```powershell
.\gga\lan-autotest.ps1 -Exe .\dist\GGA.exe -Rom C:\path\to\game.gba -Frames 130
```

For diagnostic logs, enable `GGA_DIAG=1` before running the script:

```powershell
$env:GGA_DIAG = '1'
.\gga\lan-autotest.ps1 -Exe .\dist\GGA.exe -Rom C:\path\to\game.gba -Frames 130
Remove-Item Env:\GGA_DIAG
```

This smoke test verifies startup, host/join, and sustained frame progress. It
does not synthesize keyboard or gamepad events, so it does not verify
interactive input latency.

## Linux/WSL SDL build

For core and transport changes, the smaller SDL build remains available:

```bash
sudo apt-get install -y build-essential cmake libsdl2-dev \
  zlib1g-dev libpng-dev libzip-dev
cmake -S . -B build -DBUILD_SDL=ON -DBUILD_QT=OFF \
  -DUSE_FFMPEG=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
gcc -I include gga/net-selftest.c src/gba/sio/net.c \
  -o /tmp/net-selftest -pthread && /tmp/net-selftest
```
