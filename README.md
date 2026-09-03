<p align="center">
  <img src="HC%20Player/Assets/Branding/HCPlayer.Brand.png" width="180" alt="HC Player">
</p>

<h1 align="center">HC Player</h1>

<p align="center">
  <strong>A modern Windows media player that combines the power of mpv with a native WinUI experience.</strong>
</p>

HC Player was created for users who want the playback quality and flexibility of **mpv**, without giving up a polished, native and easy-to-use Windows interface.

Instead of simply wrapping mpv in another generic frontend, HC Player integrates advanced playback features directly into a modern Windows experience.

## Why HC Player?

### Native Windows experience

HC Player is built with **Win32 and WinUI**, with native light/dark themes, Windows media controls, taskbar integration, file associations and a Fluent-style interface designed specifically for modern Windows.

### High-quality mpv playback

Under the interface is a full multimedia stack based on:

- **libmpv**
- **FFmpeg**
- **libplacebo / gpu-next**
- **D3D11**
- **WASAPI**

This provides high-quality video rendering, hardware-accelerated decoding and extensive format support.

### HDR and advanced video processing

HC Player exposes advanced playback features without requiring the user to manually edit configuration files:

- HDR playback and tone mapping
- Hardware decoding
- High-quality scaling
- Dithering and debanding
- Video interpolation
- Custom GLSL shaders
- Anime4K shader support
- Advanced color and presentation controls

### Dedicated Picture-in-Picture

HC Player includes its own **Picture-in-Picture mode**, integrated directly into the player interface, with compact playback controls and seamless return to the main window.

### Powerful subtitle support

- Primary and secondary subtitles
- Independent timing and positioning
- ASS subtitle support
- Custom subtitle fonts
- Subtitle styling controls
- External subtitle loading

### Audio control built for Windows

- WASAPI output
- Windows audio-device selection
- Optional exclusive mode
- Audio-track switching
- External audio tracks
- Audio delay controls
- Preferred audio languages

### Media information and smart badges

HC Player integrates **MediaInfo** for detailed technical information about the current media.

It can also automatically identify and display media characteristics such as:

- Dolby Vision
- Dolby Atmos
- Dolby Audio
- DTS / DTS:X
- HDR10+
- HDR
- YouTube
- HLS / Live media

Badge artwork can also be customized by the user.

### Playlist and modern playback controls

HC Player includes a complete playback queue with drag-and-drop, shuffle and direct item navigation, along with chapters, frame stepping, A–B looping, playback-speed controls, resume playback and configurable screenshots.

### Advanced without being complicated

Power users can import:

- `mpv.conf`
- GLSL shaders
- subtitle fonts
- playback profiles
- yt-dlp
- Deno

while normal users can control the most important features directly from the native Settings interface.

**yt-dlp and Deno are optional integrations and are not bundled with the HC Player installer.**

## Installation

HC Player 1.0 is distributed as a native **x64 Windows application** through the official installer.

The installer provides:

- Native Windows integration
- Custom file-type icons and associations
- Start Menu integration
- Optional Desktop shortcut
- Clean installation and uninstall behavior
- Required Windows runtime components

Portable distribution is not included in the HC Player 1.0 public release.

## Languages

- Português (Brasil)
- English (United States)

## Requirements

- Windows 11
- Windows 10 version 1809 or later
- x64 processor

## Open Source

HC Player is free and open-source software licensed under the **GNU GPL-3.0-or-later**.

Third-party components remain under their respective licenses.

See `LICENSE`, `THIRD-PARTY-NOTICES.md` and `SOURCE-MANIFEST.md` for details.

Source and compliance materials for each public release are available alongside the installer in the corresponding GitHub Release.

---

**HC Player 1.0 — native Windows design, serious mpv playback.**
