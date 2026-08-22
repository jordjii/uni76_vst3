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
- **A real VST3 host load.** JUCE's own `AudioPluginHost` was built from
  the exact pinned JUCE 9.0.1 source this project already fetches (via
  `JUCE_BUILD_EXTRAS`/`extras/AudioPluginHost`, from a throwaway CMake
  project outside this repo - not a downloaded binary), confirming the
  official host builds cleanly against this JUCE pin. Beyond that, a small
  purpose-built console harness (`UNI76HostValidator`, also built from the
  same pinned source, not part of this repository) used JUCE's real
  `AudioPluginFormatManager` / `VST3PluginFormat` hosting code - the same
  code path a DAW uses - to:
  - discover and instantiate the built `.vst3` (not `UNI76AudioProcessor`
    directly - the actual VST3-wrapped instance),
  - confirm passthrough audio + zero latency through that hosted instance,
  - confirm the hosted instance exposes all 7 UNI 76 parameters (plus one
    extra "Bypass" parameter that JUCE's VST3 wrapper adds automatically
    for every plugin that doesn't supply its own - expected, not a bug),
  - push new values through all 7 parameters via the host-facing
    `AudioProcessorParameter` interface and read them back,
  - save state, perturb every parameter, reload state, and confirm all 7
    values round-tripped correctly through the hosted instance's real
    `getStateInformation`/`setStateInformation`,
  - create the real editor (`createEditorAndMakeActive()` - genuine
    WebView2 control, not a stub), confirm it opens at the documented
    default size (960x640) and that requesting an out-of-range resize is
    clamped by the fixed-aspect-ratio constrainer to within the documented
    maximum (1350x900),
  - close and recreate the editor and confirm parameter values survive
    that cycle,
  - and hold the real, visible editor window open long enough for an
    external OS-level screenshot (`PrintWindow` with
    `PW_RENDERFULLCONTENT`, via a short PowerShell/.NET script - JUCE's own
    `Component::createComponentSnapshot()` was tried first but can't see
    WebView2's content, since it's a separately DWM-composited native
    child window, not something JUCE's own paint() call draws).

  All of the above passed. The resulting screenshot
  (`docs/screenshots/editor.png`) shows the real production UI running
  inside a real VST3 host process, with the 7 knobs at the exact test
  values (10/20/.../80%) the harness set through the host interface -
  visual confirmation that the JS <-> JUCE parameter bridge works
  end-to-end, not just that the C++ side compiles.

  This harness is a one-off validation tool, not part of the reproducible
  product build - it isn't checked into this repository.

**Embedded resources, verified in isolation.** The built `.vst3` was copied
to a location with no `Resources/Web` folder or source tree anywhere
nearby (`C:\isolated_test\UNI 76.vst3`, well outside this repository), and
the same host-validation harness ran against that copy with identical
results - confirming the WebView UI really is compiled into the binary via
`BinaryData` and the plugin has no runtime dependency on files sitting next
to the `.vst3`.

A full `pluginval` pass was **NOT TESTED** - it isn't installed on this
machine, and installing a third-party executable validator from the
internet was treated as out of scope. Given the AudioPluginHost-based
validation above already exercises real VST3 hosting, parameter
automation, state persistence, and the real editor, a `pluginval` pass is
a reasonable next step but not expected to surface anything the above
missed.
