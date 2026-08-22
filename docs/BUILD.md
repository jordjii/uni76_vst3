# UNI 76 - Build

CMake is the single source of truth for this project. Projucer is not used
and must not be reintroduced.

## Prerequisites

- CMake >= 3.25
- C++20 compiler
  - **Windows**: Visual Studio 2022 or newer, with the "Desktop development
    with C++" workload (this project was built and verified against
    Visual Studio Community 2026, MSVC 19.51 / toolset 14.51).
  - **macOS**: Xcode (recent version; not yet verified on real hardware at
    this stage - see the top-level report for what is/isn't confirmed).
- Git (JUCE is fetched automatically by CMake - see below, nothing to clone
  by hand).
- Internet access on first configure (to fetch JUCE via `FetchContent`).

### Windows: WebView2 SDK (required, one-time, per machine)

UNI 76's plugin target is configured with `NEEDS_WEBVIEW2 TRUE`, which
makes JUCE's CMake require the `Microsoft.Web.WebView2` NuGet package to be
present locally **at configure time** - this is a real, unavoidable
prerequisite of JUCE 9's CMake WebView2 integration on Windows, not
something this project's scripts can skip. WebView2Loader is statically
linked (`JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1`, set in
`Source/Plugin/CMakeLists.txt`), so once this package is present at build
time there is nothing extra to ship at runtime - no loader DLL needs to
accompany the built plugin.

Install it once per machine with PowerShell (exact command JUCE itself
documents, and what was used to verify this project's build):

```powershell
Install-PackageProvider -Name NuGet -MinimumVersion 2.8.5.201 -Force -Scope CurrentUser
Register-PackageSource -provider NuGet -name nugetRepository -location https://www.nuget.org/api/v2
Install-Package -Name Microsoft.Web.WebView2 -Scope CurrentUser -RequiredVersion 1.0.3485.44 -Source nugetRepository
```

CMake's `FindWebView2.cmake` (bundled with JUCE) looks for it under
`%USERPROFILE%\AppData\Local\PackageManagement\NuGet\Packages` by default.
If you install it somewhere else, point CMake at it with
`-DJUCE_WEBVIEW2_PACKAGE_LOCATION=<path>`.

At **runtime**, end users need the WebView2 *Runtime* (the actual browser
engine, separate from the SDK/loader above) installed - see
[Packaging/Windows/README.md](../Packaging/Windows/README.md) for the
current (unfinished) plan to detect/install it in the production installer.
Most Windows 10/11 machines already have it as a shared system component.

## Configure / build / test

Using the provided presets (recommended):

```bash
# Windows, Release
cmake --preset windows-release
cmake --build build/windows-release --config Release
ctest --test-dir build/windows-release -C Release --output-on-failure

# Windows, Debug
cmake --preset windows-debug
cmake --build build/windows-debug --config Debug
ctest --test-dir build/windows-debug -C Debug --output-on-failure
```

```bash
# macOS, Release (not yet verified on real hardware - see report)
cmake --preset macos-release
cmake --build build/macos-release --config Release
ctest --test-dir build/macos-release -C Release --output-on-failure
```

Without presets, the equivalent manual invocation is:

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The first configure fetches and builds JUCE (pinned commit - see
`cmake/PluginIdentity.cmake`) via `FetchContent`, which takes a few minutes;
subsequent configures reuse the fetched copy.

## Output

The built VST3 bundle lands at (path shown for the `windows-release`
preset; other presets/configs follow the same pattern under their own
`build/<preset>/` directory):

```
build/windows-release/Source/Plugin/UNI76_artefacts/Release/VST3/UNI 76.vst3
```

`COPY_PLUGIN_AFTER_BUILD` is intentionally off - the build never installs
itself anywhere. Use the dev-install scripts to do that explicitly:

```bash
# Windows
pwsh ./Packaging/Windows/dev-install.ps1 -Configuration Release

# macOS
./Packaging/macOS/dev-install.sh Release
```

## Tests

`Tests/` builds a small console executable (`UNI76Tests`) using JUCE's own
`juce::UnitTest` / `UnitTestRunner` (from `juce_core`) - no extra test
framework dependency. It's registered with CTest, so `ctest` (as above) is
the normal way to run it; the executable can also be run directly for more
verbose output.

Disable the test build with `-DUNI76_BUILD_TESTS=OFF` if you only want the
plugin target.

## VST3 validation

This stage's build was verified by:

- A real Release and Debug build succeeding on Windows with **zero
  compiler warnings** (`juce::juce_recommended_warning_flags` enabled).
- JUCE's own `VST3_AUTO_MANIFEST` step succeeding - this runs a small
  helper (`vst3_helper.exe`) that loads the built plugin DLL and queries
  its VST3 factory to generate `moduleinfo.json`, which is a real (if
  partial) load-and-initialise check outside of a full DAW.
- All `UNI76Tests` checks passing in both Debug and Release.

A full `pluginval` or JUCE `AudioPluginHost` pass was **NOT TESTED** -
neither tool is installed on the machine this was built on, and installing
an executable validator from the internet was out of scope for this stage
(see the top-level report for the exact reasoning). Running one of these
against the built `.vst3` above is a reasonable next step before relying on
this build in a real DAW.
