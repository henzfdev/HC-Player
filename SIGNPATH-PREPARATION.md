# SignPath preparation status — HC Player

This file records preparation work only. **It is not evidence of approved signing, a successful build, or Microsoft Store certification.**

- Preparation branch: `signpath-prep`, isolated from `main`.
- Target: HC Player 1.5.1 SEM TORRENT; no torrent components are intended for the Store distribution.
- Source archive and Inno Setup installer were supplied by the maintainer for review. Their complete synchronization and Windows build remain pending.
- Public `main` does not contain the complete 1.5.1 source or the Inno Setup project.
- The source project requires MSVC v145 and Release|x64.
- `libmpv-2.dll` is a third-party binary omitted from the repo by `.gitignore`; a verified, legally compliant dependency delivery mechanism is required.
- The Inno Setup project has hash checks. Do not modify the original `.iss` or its hash validation without a separate review.
- Optional `yt-dlp` and Deno tools are not bundled with the installer, according to the project's third-party notices.
- Before submission, verify SignPath Foundation eligibility, authenticated and reviewed GitHub Actions builds, source/license compliance, and Microsoft's signature requirements for all installed PE files.

Official references: [SignPath Foundation](https://signpath.org/), [SignPath trusted GitHub builds](https://docs.signpath.io/trusted-build-systems/github), [Microsoft Learn Store publishing](https://learn.microsoft.com/en-us/windows/apps/publish/).

**No SignPath workflow or signing secrets are configured in this branch yet.**

**Release correction:** The actual Store/SignPath candidate is 1.5.1 SEM TORRENT, incorporating the later YouTube comments text fix. Application and installer version declarations are synchronized; Inno AppId is unchanged. No signing is claimed.
