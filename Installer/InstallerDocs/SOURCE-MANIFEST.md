# HC Player 1.5.1 — Source and binary manifest

Status: HC Player 1.5.1 release manifest. Retain this file with the public
binary and corresponding-source archive.

## HC Player

- License: GPL-3.0-or-later
- Target: Windows x64
- Language: C++20 / C++/WinRT / WinUI 3
- Windows App SDK package: 2.4.0
- WinUI package: 2.3.6
- Foundation package: 2.3.9

## Shipped multimedia binary

`libmpv-2.dll`

- SHA-256: `965efde4c8199f942bf9ed9d3e6fbcb7dd9dc961524d5780a9ca67da53f14d0c`
- mpv: `v0.41.0-920-gdd5d17d32`
- mpv source revision: `dd5d17d328`
- FFmpeg: `N-125998-g2a20737f6`
- FFmpeg source revision: `2a20737f6`
- Source package: `mpv-dev-x86_64-20260809-git-dd5d17d328.7z`
- Builder: `zhongfly/mpv-winbuild` x86_64 libmpv build
- mpv feature list in the DLL includes `gpl`, `amf`, `d3d11`, `libplacebo`, `libcurl` and `vulkan`.
- The package must retain the exact corresponding source/build provenance required by the bundled GPL/LGPL components before public redistribution.

## Packaged stats overlay

`scripts/stats.lua`

- SHA-256: `7cead8a7b39a9fbd0ccb54b367dce48f65339695dbfb085b873c86ed9b93b247`
- Base: `stats.lua` from the bundled 09/08/2026 mpv/libmpv build.
- HC customization is presentation-only: automatic `target-peak=auto` output luminance (for example the recurring SDR `80 cd/m²`) is omitted from Stats.
- When `target-peak` is explicitly numeric through a profile, `mpv.conf`, or the HC `Shift+B` cycle, Stats reports the display pair as `target-peak / 1000` and `target-peak`, using the existing mpv-style formatting (for example `0.12 / 120`, `0.15 / 150`, `0.17 / 165`).
- This script does not change HDR decoding, tone mapping, target peak, target contrast or video output settings.

## Shipped MediaInfo binary

`MediaInfo.dll`

- Version: `26.05.0.0` / MediaInfoLib 26.05
- SHA-256: `a2612fa8bf639349aee9747d8a555d361f5db95b049b3af9b0c3851a21a4308d`
- Legal copyright embedded in the DLL:
  `Copyright (C) 2002-2025 MediaArea.net SARL`
- License: BSD-2-Clause

## Microsoft/NuGet components relevant to the current binary

- Microsoft.WindowsAppSDK 2.4.0
- Microsoft.WindowsAppSDK.Runtime 2.4.0
- Microsoft.WindowsAppSDK.WinUI 2.3.6
- Microsoft.WindowsAppSDK.Foundation 2.3.9
- Microsoft.Web.WebView2 1.0.3719.77 (transitive WinUI dependency; no direct HC Player WebView2 usage)

The current framework-dependent binary package contains
`Microsoft.WindowsAppRuntime.Bootstrap.dll`. WebView2 files may also be supplied
by the WinUI dependency chain and must retain their applicable notice when
redistributed.

## Corresponding-source release requirement

Before publishing the public binary, create and retain a source archive that
contains, at minimum:

1. the exact HC Player source tree used to build the release;
2. the exact build recipe/source package used for the libmpv binary above;
3. the source trees/revisions actually used for mpv, FFmpeg and every GPL/LGPL
   dependency incorporated into `libmpv-2.dll`;
4. all local patches and configuration/build scripts;
5. upstream copyright/license files for those source trees;
6. instructions sufficient to reproduce the relevant binaries.

Publish that source archive from the same release/download location as the
binary, at no additional charge. Keep it available for as long as the binary
release is offered.
