# Building HC Player

This document describes the HC Player application build. The separately built
GPL `libmpv-2.dll` is documented in `SOURCE-MANIFEST.md` and must have its own
corresponding-source archive for public releases.

## Toolchain

- Visual Studio with the MSVC `v145` toolset
- C++20
- Windows SDK restored through the project's NuGet packages
- x64 is the release architecture currently used by HC Player

The project uses `packages.config`. Restore NuGet packages before building.
Important frozen package versions include:

- Microsoft.WindowsAppSDK 2.4.0
- Microsoft.WindowsAppSDK.WinUI 2.3.6
- Microsoft.WindowsAppSDK.Foundation 2.3.9
- Microsoft.Windows.CppWinRT 3.0.260715.1
- Microsoft.Windows.SDK.BuildTools 10.0.26100.4654
- Microsoft.Web.WebView2 1.0.3719.77 (transitive WinUI dependency; no direct HC Player WebView2 usage)

## Required third-party binaries

The source tree expects the audited x64 third-party binaries in the existing
`third_party` locations used by `HC Player.vcxproj`.

Before a release build, verify their hashes against `SOURCE-MANIFEST.md`.

## Build

1. Open the HC Player solution in Visual Studio.
2. Restore NuGet packages.
3. Select `Release` and `x64`.
4. Clean Solution.
5. Rebuild Solution.
6. Launch the resulting `HC Player.exe` and perform the release smoke tests.

## Release output cleanup

Do not ship linker/debug artifacts in the public binary archive:

- `HC Player.pdb`
- `HC Player.lib`
- `HC Player.exp`

The application does not require them to run. The legal files configured in the
project are copied to the release output automatically.

## GPL release rule

Do not publish a binary-only release. The public binary and the exact
corresponding-source archive described in `SOURCE-MANIFEST.md` must be made
available together.
