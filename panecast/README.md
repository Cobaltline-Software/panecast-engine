# PanecastEngine

PanecastEngine is the screen mirroring receiver engine that the Panecast app for Windows (sold in the Microsoft Store by
Cobaltline Software) starts as a separate helper process. It is a fork of [UxPlay](https://github.com/FDH2/UxPlay) and
is licensed under the **GNU General Public License version 3** (see `LICENSE` in the repository root).

This repository is the complete corresponding source of the `PanecastEngine.exe` shipped in Panecast. Everything outside
`panecast/` is UxPlay, unmodified, at upstream commit `d9e293da6d5276315af6820d10542a859bbc3424`. The Panecast-specific
code is in this folder:

- `panecast_engine.c`: a headless receiver built on UxPlay's `lib/`. It advertises the receiver over mDNS, writes the
  decrypted video and decoded audio to a named pipe that the host app creates, reports events as JSON lines on stdout
  and reads `allow <id>`, `deny <id>` and `quit` on stdin. It has no renderer of its own.
- `CMakeLists.txt` and `build-windows.sh`: the Windows build.

The Panecast app itself is a separate, closed-source program. It is not linked with the engine and talks to it only
through the pipe and stdin/stdout described above.

## Building on Windows

The release builds use MSYS2 UCRT64 with these package versions:

| Component | Version | How it ends up in the release |
| --- | --- | --- |
| mingw-w64-ucrt-x86_64-gcc | 16.2.0-6 | compiler |
| mingw-w64-ucrt-x86_64-cmake | 4.4.4-3 | build tool |
| mingw-w64-ucrt-x86_64-openssl | 3.6.5-1 | `libcrypto-3-x64.dll` (Apache-2.0) |
| mingw-w64-ucrt-x86_64-libplist | 2.7.0-4 | `libplist-2.0.dll` (LGPL-2.1+) |
| mingw-w64-ucrt-x86_64-winpthreads | 14.0.0.r495.g63ebe6cdb-1 | `libwinpthread-1.dll` (MIT/ZPL) |
| FFmpeg | `n7.1.2` (`f893221c8d89cb798b829bebe71d55e1a3f242fd`) | static: AAC and ALAC decoders, libswresample (LGPL-2.1+) |

The sources of the MSYS2 packages are published by the MSYS2 project (https://github.com/msys2/MINGW-packages).

```
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,openssl,libplist} make nasm diffutils
git clone https://github.com/Cobaltline-Software/panecast-engine
git clone --branch n7.1.2 --depth 1 https://github.com/FFmpeg/FFmpeg ffmpeg-src
cd panecast-engine
MSYSTEM=UCRT64 ./panecast/build-windows.sh ../ffmpeg-src
```

The script builds a minimal static FFmpeg (only LGPL code, configure flags are in the script), then the engine, and
copies `PanecastEngine.exe` and the DLLs it imports to `%LOCALAPPDATA%\PanecastBuild\out`.

## Releases

Each Panecast release that changes the engine is tagged here as `panecast-<engine version>`. Panecast 1.0 ships the
engine built from tag `panecast-1.0.0`.
