# HC Player 1.0 — GPL release checklist

This checklist is intentionally conservative. A checked application build is
not automatically a legally complete public release.

- [ ] Release x64 build completes with 0 errors.
- [ ] Player opens and passes playback/Settings/theme/DPI smoke tests.
- [ ] `Netflix Sans Medium.otf` is absent from source and binary packages.
- [ ] `HC Player.pdb`, `.lib` and `.exp` are absent from the public binary ZIP.
- [ ] `LICENSE`, `THIRD-PARTY-NOTICES.md`, `SOURCE-MANIFEST.md` and `Licenses/`
      are present in the public binary package.
- [ ] Exact HC Player source tree is archived/tagged for the release.
- [ ] Exact libmpv corresponding-source bundle is archived, including the
      pinned build recipe, mpv/FFmpeg/dependency sources, patches and licenses.
- [ ] Binary and corresponding source are offered together from the release
      location at no additional charge.
- [ ] If WebView2 files are present through the WinUI dependency chain, the
      WebView2 BSD notice is retained in the public binary package.
- [ ] Windows App SDK redistributed files are reviewed against the exact 2.4.0
      Microsoft license terms.
- [ ] libdvdcss/anticircumvention distribution policy is reviewed for target
      channels/jurisdictions.
- [ ] Dolby/DTS/YouTube/HDR10+ naming and trademark presentation are reviewed.
- [ ] Codec-patent considerations for the intended distribution model/regions
      are reviewed separately from GPL compliance.
