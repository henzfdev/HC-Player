# HC Player 1.0 — Source and binary manifest

Status: release-candidate manifest. Freeze this file with the final public
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

- SHA-256: `38acd030006062830d792a958c5f2adc293fa965996797980ec73d9610c309f4`
- mpv: `v0.41.0-85-g468d34c9b`
- mpv source revision: `468d34c9b`
- FFmpeg: `N-122476-g685ceebd4`
- FFmpeg source revision: `685ceebd4`
- mpv-winbuild-cmake recipe revision:
  `d4e9628f2d67410ed1b93c8237a37b3c3368d9e0`
- mpv feature list in the DLL includes `gpl`.
- Audited FFmpeg recipe uses `--enable-gpl --enable-version3` and does not use
  `--enable-nonfree`.

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
2. the exact mpv-winbuild-cmake recipe revision above;
3. the source trees/revisions actually used for mpv, FFmpeg and every GPL/LGPL
   dependency incorporated into `libmpv-2.dll`;
4. all local patches and configuration/build scripts;
5. upstream copyright/license files for those source trees;
6. instructions sufficient to reproduce the relevant binaries.

Publish that source archive from the same release/download location as the
binary, at no additional charge. Keep it available for as long as the binary
release is offered.
