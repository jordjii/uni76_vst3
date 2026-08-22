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

- **HTML/CSS-first.** The GUI is built entirely in HTML + CSS + vanilla
  JavaScript (`Resources/Web/`), compiled into the plugin binary as
  `BinaryData` - never loaded from disk at runtime.
- **No raster UI assets.** No PNG/JPG, no SVG illustrations, no Canvas
  drawing, no CDN, no Google Fonts/external fonts, no external JS
  libraries or frameworks, no 3D/photoreal rendering. Knobs, ticks, scales
  and meters are all real DOM elements positioned/rotated with CSS custom
  properties and `transform` - see `Resources/Web/knobs.css` +
  `Resources/Web/knob.js`. The only permitted decorative effect beyond flat
  colour is very low-opacity CSS-generated grain/gradient (see the
  `.app::after` rule in `shell.css`) - never anything that reads as 3D.
- **One primary knob per processor.** Each of the 7 modules gets exactly
  one large interactive knob bound to its one public parameter. Any
  secondary read-out (the Preamp filter-position lines, the tri-point
  scales on EQ/Pitch/Pan/Verb/Imager) is purely a derived visual computed
  from that same parameter's value in JS - never a second control, never a
  second parameter.
- **7 immutable public parameters.** See below - the UI must never grow an
  8th knob or a new APVTS parameter without a deliberate, separate
  decision.
- No network requests from the frontend, ever. The one vendored exception
  to "no external JS" is `Resources/Web/juce_webview.js`, an unmodified
  local copy of JUCE's own `@juce-framework/webview` frontend module -
  it's part of the framework's own native bridge mechanism, not a
  third-party dependency.
- CSS is split by concern (`tokens` / `reset` / `shell` / `header` /
  `modules` / `knobs` / `scales` / `meters` / `responsive`), all sized in
  relative units (`rem`, `vw`, `clamp()`) rather than fixed pixels, so the
  UI scales with the editor instead of being pinned to a canvas size. See
  `Source/UI/WebUIEditor.cpp` for the resizable 3:2-locked window
  (600x400 min / 960x640 default / 1350x900 max) this is designed to fill.

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

**Stage: technical foundation + production UI complete for this pass. No
DSP - deliberately.**

Verified on this machine (Windows, Visual Studio Community 2026 /
MSVC 19.51):

- Clean configure + build for both `windows-debug` and `windows-release`
  presets, **zero compiler warnings**.
- Real `.vst3` produced at
  `build/windows-release/Source/Plugin/UNI76_artefacts/Release/VST3/UNI 76.vst3`.
- All `UNI76Tests` (JUCE `UnitTest`-based) pass in both Debug and Release:
  processor construction, mono/stereo bus layout negotiation, all 7
  parameter IDs present at 50% default, state save/modify/restore
  round-trip, passthrough audio-buffer integrity across multiple sample
  rates/block sizes with zero reported latency, the saved-state XML
  schema/shape contract, and exact 0%/50%/100% normalised<->percent
  conversion.
- **Real VST3 host validation.** JUCE's own `AudioPluginHost` built
  cleanly from this project's exact pinned JUCE 9.0.1 source. A
  purpose-built harness using the same real `AudioPluginFormatManager` /
  `VST3PluginFormat` hosting code then loaded the built `.vst3`,
  round-tripped all 7 parameters and full state through the *hosted*
  instance (not `UNI76AudioProcessor` directly), opened the real WebView2
  editor at its documented default size, confirmed the resize constrainer
  clamps to the documented maximum, closed/reopened the editor, and held
  the real window open for an OS-level screenshot
  (`docs/screenshots/editor.png`) - which shows the 7 knobs at the exact
  test values the harness set through the host, visually confirming the JS
  <-> JUCE bridge works end-to-end. See docs/BUILD.md for the full detail
  and why this is stronger evidence than the previous entry's
  `moduleinfo.json`-only check.

Not verified (say so plainly rather than guessing):

- **macOS**: not built or tested - no macOS machine available in this
  session. `CMakePresets.json` defines `macos-debug`/`macos-release`
  presets targeting a Universal Binary (`x86_64;arm64`), but they are
  unverified.
- **pluginval**: not installed on this machine; installing a third-party
  executable validator from the internet was treated as out of scope. The
  AudioPluginHost-based validation above already covers real VST3 hosting,
  parameter automation, and state persistence, so this is a lower-priority
  follow-up than it was in the previous entry.
- **Windows/macOS production installers**: both `Packaging/*` folders are
  documented skeletons, not working installers (see
  [docs/RELEASE.md](docs/RELEASE.md)).
- **Manual mouse-driven interaction** (literally dragging a knob with a
  mouse) wasn't exercised - the validation harness drives parameter
  changes through the host API, which exercises the same JS<->JUCE sync
  path a real drag would, but isn't a substitute for a human trying the
  actual pointer/wheel/keyboard interactions in a real DAW.

## Next steps (not started - waiting for a separate go-ahead)

DSP for any of the 7 modules, final visual design details beyond the
approved concept, presets browser, copy protection, licensing system. See
[docs/RELEASE.md](docs/RELEASE.md) for the pre-public-release checklist.
