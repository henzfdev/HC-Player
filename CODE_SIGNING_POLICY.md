# Code signing policy — HC Player

**Status: preparation in progress; SignPath Foundation has NOT approved this project yet.**

This policy describes the intended code-signing and release safeguards for [HC Player](https://github.com/henzfdev/HC-Player). Existing downloads are not represented as signed by SignPath.

## People and responsibilities

- Maintainer, code reviewer, and signing approver: [henzfdev](https://github.com/henzfdev).
- Changes submitted by external contributors must be reviewed by the maintainer before incorporation into an official release.
- The maintainer reviews each official signing request separately and may reject it.
- All accounts permitted to release or approve signatures must use multi-factor authentication.

## Release and signing controls

The following controls are requirements for the planned signing workflow; they are **not yet operational**:

1. Use the designated release source revision from the public repository, with traceable source-to-artifact provenance.
2. Build on GitHub-hosted Actions runners with dependencies and build tools verified and recorded.
3. Archive release source revision, installer scripts, dependency versions and hashes with each release.
4. Require explicit human approval for every production signing request.
5. Store any signing authorization tokens in GitHub Actions secrets; never commit credentials.
6. Publish installers and their SHA-256 hashes on the [official releases page](https://github.com/henzfdev/HC-Player/releases).
7. Respect third-party software authorship and licensing: do not claim upstream executable binaries were authored by HC Player or sign them as such. Review target channel requirements separately.
8. Suspend release or signing if a security problem requires investigation.

Production signing is contingent on application acceptance by SignPath Foundation and a verified automated build. Prior or unsigned downloads do not retrospectively become signed.

If accepted and operational, the project will acknowledge:

> Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

The code-signing certificate, if granted through this program, would identify SignPath Foundation rather than the HC Player maintainer.

## User privacy and software behavior

HC Player provides local playback and optional access to external online services. It does not intentionally transmit user telemetry to developer-controlled servers. Third-party providers may receive technical connection information when requested features are used. See the [Privacy policy](PRIVACY.md).

The Windows installer may require administrative privileges to install runtime prerequisites and file associations. Uninstall options and user data handling must be tested before any signed release.

## Contact

Privacy, security and signing questions: henzfdev@gmail.com
