# SignPath activation — current technical status

**This branch is for safe experiments only. No main-branch changes, signing or publishing have been performed.**

## Implemented in `signpath-prep`

- Signing policy and bilingual privacy policy.
- Static Windows hosted preflight (passed once).
- A toolchain / NuGet restore workflow (runs on `signpath-prep` pushes).
- Manual, fail-closed unsigned application build workflow: `.github/workflows/signpath-build-unsigned.yml`.

## Remaining mandatory steps

1. Commit the original **13 files** from the provided HC Player 1.5.1 SEM TORRENT `Installer/` directory to this preparation branch; the only pre-existing tracked file in `Installer/` is `CI-PREPARATION.md`. Do not change `main`.
2. Provide an immutable URL to **the exact** `libmpv-2.dll` (SHA-256 `965efde4c8199f942bf9ed9d3e6fbcb7dd9dc961524d5780a9ca67da53f14d0c`), e.g. publish as a versioned GitHub release asset, together with the source/compliance materials.
3. Verify the original Inno Setup 7 compiler binary on GitHub runners for the optional installer stage. No unsigned or older substitute should silently be used.
4. Run the unsigned manual build after 1–3 and test it on Windows. A passing static check does not mean the executable compiles.
5. Finish SignPath Foundation application and let the foundation evaluate the project. Then add SignPath's actual Organization ID, project slug, signing-policy slug, artifact config, secret API token, GitHub App and **per-release human approval**, as required by [SignPath's GitHub instructions](https://docs.signpath.io/trusted-build-systems/github). No signing request should be programmed before that.
6. Review third-party DLL signatures and license compliance independently of whether SignPath accepts the project. It is not permissible to sign third-party binaries as if authored by HC Player.
7. After signing, validate silent/offline installer, uninstall/upgrade, and Microsoft Store certification requirements. Do **not** publish a test artifact as a stable release.

## Important upstream and installer invariants

- Inno `.iss` unchanged SHA256 `0f2d3151b98ce074da1ef0057b0019c74ad583d699b734ca9bf71ea9733c7251`.
- SignPath's free OSS program signs binaries of the maintainers' own project, with upstream rules.
- `yt-dlp` and Deno are optional externally imported tools and are **not bundled**.
- Windows App SDK/WinUI, libmpv and MediaInfo remain under their third-party licenses.
- No workflow in this branch changes `main` or creates public releases.

## Running the isolated build without touching main

A `workflow_dispatch` button is not available to workflows that exist only on non-default branches (GitHub restriction). To avoid any change to `main`, the unsigned build workflow also responds to pushes on `signpath-prep`, but **only** after this repository Actions variable is set:

- `HC_LIBMPV_URL` = a versioned release-asset URL in `https://github.com/henzfdev/HC-Player/releases/download/<version>/...` returning the exact original `libmpv-2.dll`.
- Its SHA256 is hard-coded and checked in the build workflow. A wrong or repackaged DLL always fails the job.
- Optional `HC_SIGNPATH_BUILD_INSTALLER=true` requests packaging. It fails closed unless original Installer source files, matching hashes, and Inno Setup 7 are present.

No public artifact is uploaded as a release. CI artifacts, if built, are labeled UNSIGNED and expire after three days. SignPath signature activation remains a separate, later operation requiring the user's account and foundation approval.

**Release correction:** The actual Store/SignPath candidate is 1.5.1 SEM TORRENT, incorporating the later YouTube comments text fix. Application and installer version declarations are synchronized; Inno AppId is unchanged. No signing is claimed.
