# UNI 76 - project journal

This file is the living technical log for UNI 76. Keep it up to date as the
project moves forward; it is the fastest way for anyone (human or AI) to
get oriented without re-reading the whole codebase.

## What this is

- **Brand**: Nostalgia Audio
- **Product**: UNI 76
- **Purpose**: a universal vintage/analogue-style audio effect processor
  (not a synth).
- **Platforms**: Windows x64, macOS (Intel x86_64 + Apple Silicon arm64,
  built as a Universal Binary).
- **Format policy**: VST3 only. Do not add AU, AAX, VST2, LV2, or
  Standalone to the product build without a deliberate, separate decision -
  see `FORMATS` in `Source/Plugin/CMakeLists.txt`.

## Build architecture

- **CMake is the single source of truth.** Projucer is not used and must
  not be reintroduced.
- **JUCE 9**, fetched via `FetchContent` pinned to a specific commit (see
  `cmake/PluginIdentity.cmake`, `UNI76_JUCE_GIT_TAG`) for reproducible
  builds. Do not switch this to a floating branch.
- **AudioProcessorValueTreeState** is the parameter system.
  `juce::WebBrowserComponent` (+ `WebSliderRelay` /
  `WebSliderParameterAttachment`) is the UI bridge. Windows uses the
  WebView2 backend (statically-linked loader); other platforms fall back to
  their native WebView automatically.
- See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the folder-by-folder
  breakdown and [docs/BUILD.md](docs/BUILD.md) for exact build commands.

## HTML/CSS/JS UI rule

The GUI is built entirely in HTML + CSS + JavaScript (`Resources/Web/`),
compiled into the plugin binary as `BinaryData` - never PNG/JPG interface
art, never photorealistic graphics, and never loaded from disk at runtime.
No CDN, no Google Fonts, no external JS libraries, no network requests from
the frontend. The one vendored exception is `Resources/Web/juce_webview.js`,
an unmodified local copy of JUCE's own `@juce-framework/webview` frontend
module - it's part of the framework's own bridge mechanism, not a
third-party dependency.

## Immutable identity (do not change after first public release)

| | |
|---|---|
| Company | Nostalgia Audio |
| Product | UNI 76 |
| Bundle ID | `com.nostalgiaaudio.uni76` |
| Plugin manufacturer code | `Nsta` |
| Plugin code | `Uni6` |
| Version (current) | `0.1.0` |

Single source of truth: [`cmake/PluginIdentity.cmake`](cmake/PluginIdentity.cmake).
Do not duplicate these values by hand elsewhere - `include()` that file, or
read from `Source/Core/PluginIdentity.h` for the C++-only schema-version
constant.

### The 7 public parameters (stable IDs, do not rename)

`preamp`, `eq`, `saturation`, `pitch`, `panorama`, `reverb`, `imager` -
see [`Source/Parameters/ParameterIDs.h`](Source/Parameters/ParameterIDs.h).
All are `0..100%`, default `50%`. Low Cut / High Cut are **not** separate
parameters - they will be internal to the future Preamp DSP module.

## Realtime audio-thread rules

`AudioProcessor::processBlock()` must stay a strictly transparent
passthrough at this project stage: input == output, no gain, no latency, no
DSP, **no allocations, no locks, no file I/O, no calls into the
WebView/GUI layer**. When real DSP is eventually wired in, these rules
still apply to whatever runs on the audio thread - allocate/lock/log on the
message thread or in `prepareToPlay`, never in `processBlock`.

## Don't change working architecture without a reason

If something here works and is documented, don't refactor it "for
cleanliness" without a concrete reason tied to a real requirement. In
particular:

- Don't reintroduce Projucer.
- Don't move off the WebView-based UI bridge to a hand-rolled polling
  protocol - JUCE's relay/attachment mechanism already solves
  bidirectional sync correctly.
- Don't add DSP formats/effects beyond the 7 parameters without an explicit
  decision to start that stage of work.
- Don't add AU/AAX/VST2/LV2/Standalone targets without a reason.

## Current status (as of this entry)

**Stage: technical foundation complete for this pass. No DSP, no final UI
design - deliberately.**

Verified on this machine (Windows, Visual Studio Community 2026 /
MSVC 19.51):

- Clean configure + build for both `windows-debug` and `windows-release`
  presets, **zero compiler warnings**.
- Real `.vst3` produced at
  `build/windows-release/Source/Plugin/UNI76_artefacts/Release/VST3/UNI 76.vst3`.
- JUCE's VST3 auto-manifest step (`vst3_helper.exe`) successfully loaded
  the built plugin and generated `moduleinfo.json` - a real, if partial,
  load/initialise check outside of a DAW.
- All `UNI76Tests` (JUCE `UnitTest`-based) pass in both Debug and Release:
  processor construction, mono/stereo bus layout negotiation, all 7
  parameter IDs present at 50% default, state save/modify/restore
  round-trip, and passthrough audio-buffer integrity across multiple
  sample rates/block sizes with zero reported latency.

Not verified (say so plainly rather than guessing):

- **macOS**: not built or tested - no macOS machine available in this
  session. `CMakePresets.json` defines `macos-debug`/`macos-release`
  presets targeting a Universal Binary (`x86_64;arm64`), but they are
  unverified.
- **pluginval / AudioPluginHost**: neither is installed on this machine;
  downloading an executable validator from the internet was treated as out
  of scope. Not run.
- **Windows/macOS production installers**: both `Packaging/*` folders are
  documented skeletons, not working installers (see
  [docs/RELEASE.md](docs/RELEASE.md)).

## Next steps (not started - waiting for a separate go-ahead)

DSP for any of the 7 modules, final visual design, presets browser, copy
protection, licensing system. See [docs/RELEASE.md](docs/RELEASE.md) for
the pre-public-release checklist.
