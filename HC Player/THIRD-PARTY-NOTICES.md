# HC Player — Third-party notices

HC Player is free software. The HC Player source code is licensed under the
**GNU General Public License, version 3 or (at your option) any later version
(GPL-3.0-or-later)**. The complete GPL v3 text is provided in `LICENSE`.

Third-party components keep their own copyright notices and license terms.
Nothing in the HC Player license changes the license of a separately licensed
third-party component.

## Components shipped in the current Windows binary

### libmpv / mpv multimedia stack

The shipped `libmpv-2.dll` is the audited GPL build identified in
`SOURCE-MANIFEST.md` and `Licenses/mpv-FFmpeg-NOTICE.txt`.

- mpv: GPL-2.0-or-later by default; this binary reports the `gpl` feature.
- FFmpeg in this binary: configured with `--enable-gpl --enable-version3`, so
  the resulting FFmpeg combination is GPL-3.0-or-later.
- libplacebo: LGPL-2.1-or-later.
- The DLL also contains or links additional libraries. The exact corresponding
  source archive for the release is the authoritative inventory and must retain
  every applicable upstream copyright notice, license file, patch and build
  script.

FFmpeg licensing documentation requires attribution to the Independent JPEG
Group for relevant incorporated code. Accordingly:

> This software is based in part on the work of the Independent JPEG Group.

### MediaInfo / MediaInfoLib 26.05

Copyright (c) 2002-2025 MediaArea.net SARL. All rights reserved.
Distributed under the BSD 2-Clause license. The full notice is included in
`Licenses/MediaInfo-BSD-2-Clause.txt`.

### Microsoft WebView2 1.0.3719.77

HC Player does not instantiate or use a WebView2 control directly. WebView2 is
restored as a dependency of the Microsoft.WindowsAppSDK.WinUI package used by
the application. When WebView2 files are present in the distributed build, the
BSD-style 3-Clause notice remains included in
`Licenses/WebView2-BSD-3-Clause.txt`.

### Microsoft Windows App SDK 2.4.0 / WinUI 3

The current unpackaged, framework-dependent release contains
`Microsoft.WindowsAppRuntime.Bootstrap.dll`. This Microsoft component is not
covered by the HC Player GPL; it remains governed by Microsoft's own Windows
App SDK license terms. See
`Licenses/Microsoft-WindowsAppSDK-2.4.0-NOTICE.txt`.

The project currently restores Windows App SDK 2.4.0 with WinUI 2.3.6 and
Foundation 2.3.9.

## Optional integrations not shipped with HC Player

`yt-dlp` and Deno can be selected/imported by the user for online-media
features. They are not included in the current HC Player binary package and are
therefore not being redistributed by this release candidate.

## Trademarks

Names such as Dolby, DTS, YouTube and HDR10+ may be trademarks of their
respective owners. Their appearance in HC Player is descriptive of detected
media technology and does not imply endorsement or certification. Trademark
review remains separate from open-source license compliance.

## Corresponding source

A public binary release must be accompanied by equivalent access to the exact
corresponding source for HC Player and for the GPL/LGPL code incorporated into
the shipped `libmpv-2.dll`, including the build scripts and patches needed to
produce that binary. `SOURCE-MANIFEST.md` records the audited revisions and
hashes. A list of upstream URLs by itself is not a substitute for the required
corresponding-source archive.
