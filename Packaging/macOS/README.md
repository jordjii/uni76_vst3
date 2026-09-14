# macOS packaging

## Current status: unsigned .pkg builds and installs; signed/notarized pipeline still not started

The macOS build (Universal Binary, x86_64 + arm64) was compiled and
packaged for the first time on real macOS hardware (Xcode 26.6, CMake
4.4.3, macOS 26.6.2) via `cmake --preset macos-release` +
`cmake --build build/macos-release --config Release --target UNI76_VST3`,
producing a real `UNI 76.vst3` (confirmed Mach-O universal x86_64/arm64,
ad-hoc signed by Xcode's default build settings) and a real, installable
**unsigned** `.pkg` via `build-pkg-unsigned.sh` below. No Apple Developer
ID has been used - the signed/notarized pipeline (`build-pkg.sh`) is still
exactly the untested skeleton described below. One unrelated, pre-existing
gap this surfaced: `Tests/SoakTest.cpp` (a standalone diagnostic
executable, not part of the plugin or the routine `UNI76Tests` suite) hard-
includes `windows.h` with no platform guard, so `cmake --build` with no
`--target` (building every target, `UNI76Soak` included) fails on macOS;
building the actual plugin target (`--target UNI76_VST3`) is unaffected
and succeeds cleanly. Not fixed here (out of scope for packaging) - flagged
as a follow-up.

**AU added as a second macOS format** (same session, follow-up round) -
`FORMATS` is now `VST3 AU` on macOS (`VST3` only on Windows, since AU is
Apple-only) - see the "Format policy" section of the top-level CLAUDE.md
for the full reasoning (a stray, out-of-policy AU build already on this
dev Mac was confusing FL Studio's plugin scan against the fresh VST3; the
user explicitly chose to make AU a real second format rather than just
delete the stray copy). `build-pkg-unsigned.sh` now packages both bundles
into one `.pkg` in a single `pkgbuild`/`productbuild` pass.

- [`dev-install.sh`](dev-install.sh) - copies a locally built `.vst3` into
  the current user's `~/Library/Audio/Plug-Ins/VST3` for development use.
- [`build-pkg-unsigned.sh`](build-pkg-unsigned.sh) - **works today.** Takes
  the built `.vst3` and `.component` paths and builds a real, installable
  `.pkg` (via `pkgbuild` + `productbuild`, no `--sign`) that installs
  `UNI 76.vst3` to the system-wide `/Library/Audio/Plug-Ins/VST3` and
  `UNI 76.component` (AU) to `/Library/Audio/Plug-Ins/Components`. Not
  signed or notarized, so Gatekeeper blocks a double-click on first run -
  installing it needs an explicit override (right-click the `.pkg` ->
  Open -> Open, or System Settings > Privacy & Security > "Open Anyway"
  after the first blocked attempt). Intended as a local-use stand-in
  until real Developer ID credentials exist, not a replacement for the
  production pipeline below.
- [`build-pkg.sh`](build-pkg.sh) - a **documented skeleton** for the future
  signed/notarized `.pkg` pipeline. It refuses to run to completion (exits
  1) until real Developer ID credentials are supplied via environment
  variables - it must never be able to silently "succeed" while faking
  signing or notarization.

## Planned production pipeline

1. **Codesign** the `.vst3` bundle with a `Developer ID Application`
   identity, hardened runtime enabled.
2. **Package** with `pkgbuild` (component package) then `productbuild`
   (product archive), signed with a `Developer ID Installer` identity,
   installing to the system-wide `/Library/Audio/Plug-Ins/VST3`.
3. **Notarize** via `xcrun notarytool submit --wait`, using credentials
   stored with `xcrun notarytool store-credentials` (a keychain profile
   name, referenced by name - never a raw Apple ID password or
   app-specific password in any script or CI config file).
4. **Staple** the notarization ticket with `xcrun stapler staple` so the
   installer works offline/without network access at install time.

## Secrets policy

No certificates, private keys, Apple ID credentials, app-specific
passwords, or notarytool keychain profile contents may ever be committed to
this repository. `build-pkg.sh` only ever reads identity/profile *names*
from environment variables supplied at run time (by a developer's local
keychain or by CI secrets) - never values that are secret themselves.

## Universal Binary

The CMake build (`cmake/PluginIdentity.cmake` + root `CMakeLists.txt`)
already targets `CMAKE_OSX_ARCHITECTURES = x86_64;arm64` by default on
macOS, so a single `.vst3` produced by the `macos-release` preset should be
a Universal Binary covering both Intel and Apple Silicon. This has not been
verified on real macOS hardware yet - see the top-level report for what is
and isn't confirmed.
