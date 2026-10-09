# Installer file synchronization (SignPath preparation only)

Work **only on `signpath-prep`**. Never switch to or commit on `main`.

The 13 Installer inputs from the original HC Player 1.5.1 SEM TORRENT ZIP must be copied to the repository's top-level `Installer/` folder, preserving filenames and directory structure. Copy the original content; do not edit or regenerate the `.iss`, PowerShell scripts or `.ico`.

The root `.gitignore` already excludes generated binaries under `Installer/Output/`, `Installer/Payload/` and `Installer/Prerequisites/`. The root `.gitattributes` pins LF text line endings, preserving the original raw SHA-256.

The GitHub Actions preflight validates all **13 exact SHA-256 hashes** after the installer source is present; a partial or modified upload fails closed. Its previous green status before the source was uploaded did **not** prove installer readiness.

The exact files and the expected hashes are recorded in `.github/workflows/signpath-preflight.yml`. Run the preflight after the installer-source commit. A passing preflight does not compile, sign or certify an application.

The original `libmpv-2.dll` is *not* included here, and is not to be committed to source control as an unverified replacement.
