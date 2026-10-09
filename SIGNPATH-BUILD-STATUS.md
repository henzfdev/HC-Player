# HC Player 1.5.1 SEM TORRENT — SignPath and Microsoft Store readiness

**As of 2026-10-09.** Only `signpath-prep` contains the Store signing preparation. **Do not modify or merge into `main` without express maintainer authorization.**

## Verified milestones (not signed)

- **Build and SAFE Inno Setup success:** GitHub Actions run [37895878252](https://github.com/henzfdev/HC-Player/actions/runs/37895878252), commit `992ebad50feeb3167494c743e62257de18720101`. All steps completed successfully.
- Visual Studio 2026 (MSVC v145), Windows-hosted build, NuGet restore, C++/WinUI `Release|x64`, and EXE `1.5.1.0` version metadata passed.
- The exact original `libmpv-2.dll` (`965efde4c8199f942bf9ed9d3e6fbcb7dd9dc961524d5780a9ca67da53f14d0c`) was obtained via an **unpublished draft-release ZIP**, SHA-256 verified; this must remain draft pending licensing review.
- The original `scripts/stats.lua` SHA-256 (`7cead8a7b39a9fbd0ccb54b367dce48f65339695dbfb085b873c86ed9b93b247`) was verified and restored byte-exact from the Git blob, without changing playback behavior.
- Official Inno Setup 7.1.0 installer Authenticode signature checked, followed by the original `GERAR-INSTALADOR-V1.ps1`, `STAGE-PAYLOAD.ps1`, `CHECK-PAYLOAD.ps1` and `BUILD-INSTALLER.ps1` SAFE pipeline.
- Output during the successful run: `HC_Player_1.5.1_x64_Setup.exe`; its SHA-256, version `1.5.1.0`, prerequisite Microsoft signatures, payload and original Inno `.iss` checks passed. The permanent Inno AppId was kept for upgrades.
- **The successful workflow intentionally did not upload/store the installer as a GitHub artifact and did not create a release.** The compiled EXE/Setup from this ephemeral runner is *not retrievable as an Actions artifact*. This is not a downloadable release or a smoke test on customer Windows.
- The `main` branch is unchanged from `101023800da772fe91899622f34ec0598a569fcd`.

## Mandatory work before an official signed 1.5.1 release

1. **Submit the SignPath Foundation open-source signing application:** https://signpath.org/apply.html . The project has *not* been approved. The maintainer must provide/confirm project information and formally submit the application. The public source repository is https://github.com/henzfdev/HC-Player ; the 1.5.1 candidate is at https://github.com/henzfdev/HC-Player/tree/signpath-prep . The code-signing policy and bilingual privacy policy are linked from that branch's README.
2. **Resolve licensing/distribution prerequisites.** The `libmpv-2.dll` is third-party GPL/LGPL-related code. Confirm exact corresponding source/build recipe and redistributable licenses before making a public artifact. The Foundation's free signing terms allow unsigned upstream OSS binaries in a signed package but do not allow signing unowned third-party PE files as HC Player's own work; Microsoft Store Win32 installer rules may require PE signatures too. Investigate this conflict explicitly.
3. **After approval**, configure the official SignPath Github App/trusted build system, Organization ID, project slug, artifact configuration, signing policy, securely stored API token and a human approver per release. Never invent these values; do not submit a signing request before approval.
4. **Design a controlled artifact flow.** The SignPath GitHub connector requires the UNSIGNED installer to be uploaded as a GitHub Actions artifact by the same build workflow before submission (actions/upload-artifact v4+). This has deliberately NOT been enabled yet, to avoid premature artifact distribution. Signed installers, validation artifacts and SHA-256 values must be preserved and audited after signing.
5. **Sign and certify only after those prerequisites:** check whether SignPath artifact configuration covers the HC Player EXE and Inno installer without improperly signing third-party DLLs. Test clean installation, 1.5.0-to-1.5.1 upgrade, uninstall, relevant integrations and Windows Store MSI/EXE certification. Do not distribute unsigned QA output as a stable release.

## Preserved invariants

- HC Player 1.5.1 **SEM TORRENT** only, no torrent source or engine included in the Store candidate.
- `yt-dlp` and Deno are user-imported optional external tools and are not bundled in the installer.
- Do not remove or weaken SHA-256 verification, do not change media engine or UI animations, and keep PT-BR/EN-US in sync.
- No signing token in Git, no automatic public release, no changes to `main`.

Authoritative references: [SignPath Foundation terms](https://signpath.org/terms.html), [SignPath GitHub origin-verified signing](https://docs.signpath.io/trusted-build-systems/github), [Microsoft Windows installer signing](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options).
