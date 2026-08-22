# macOS packaging

## Current status: NOT TESTED, foundation only

No macOS machine or Apple Developer ID has been used to build, sign, or
notarize UNI 76 as part of this stage - do not read anything in this folder
as evidence that macOS packaging works. What exists:

- [`dev-install.sh`](dev-install.sh) - copies a locally built `.vst3` into
  the current user's `~/Library/Audio/Plug-Ins/VST3` for development use.
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
