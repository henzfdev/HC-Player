# Inno Setup source for SignPath preparation

**Status: source upload pending. No code signing or Microsoft Store readiness is claimed.**

This branch is configured to track the original Inno Setup 7 *source* directory `Installer/`. The source was supplied as part of the HC Player 1.5.0 SEM TORRENT source ZIP but has **not** yet been committed to GitHub. Only copy source templates, PowerShell scripts, documentation, and `InstallerAssets/HCPlayer_Setup.ico`.

**Original unmodified SHA-256 hashes:**

| Input | SHA-256 |
| --- | --- |
| `Installer/HC_Player_1.5.0_x64.iss` | `f36067a3f2922857f95dfb53a3780fee55aa8e6d6cf28f2c431f436310c3bac3` |
| `Installer/InstallerAssets/HCPlayer_Setup.ico` | `f540bb0f98165dc1a90684bd4d1cbed29adaacff708b5364435b5307dcace773` |
| Required `libmpv-2.dll` | `965efde4c8199f942bf9ed9d3e6fbcb7dd9dc961524d5780a9ca67da53f14d0c` |
| Required `MediaInfo.dll` | `a2612fa8bf639349aee9747d8a555d361f5db95b049b3af9b0c3851a21a4308d` |

Do not commit generated `Installer/Payload/`, `Installer/Prerequisites/`, or `Installer/Output/` binaries. The `.gitignore` patterns protect these paths, while allowing their README placeholders.

The original installer scripts intentionally enforce a strict hash of the `.iss` file. Do not casually edit the `.iss` or bypass its check. If changes are required, review them separately against the stable template.

The preflight GitHub Actions workflow is **unsigned, read-only and does not build an installer**. The next stage requires verifying dependencies and getting the original Inno sources into the preparation branch.

All changes must remain on `signpath-prep`, not `main`.
