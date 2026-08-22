# Windows packaging

## Current status: foundation only

There is no working production installer yet. What exists:

- [`dev-install.ps1`](dev-install.ps1) - copies a locally built `.vst3` into
  `%LOCALAPPDATA%\Programs\Common\VST3` for development use. This is **not**
  run automatically by the build (`COPY_PLUGIN_AFTER_BUILD` is off).
- [`UNI76.iss`](UNI76.iss) - an Inno Setup **skeleton**. It compiles the
  shape of a real installer (identity block, install location, WebView2
  Runtime-check TODO) but does not yet package a real build artefact.

## Why Inno Setup

Inno Setup was chosen over WiX/MSIX because:

- It handles a straightforward "copy a VST3 bundle into
  `Common Files\VST3`, needs admin" install with very little script, which
  is all a VST3-only plugin installer needs at this stage.
- It's free, scriptable, and has no dependency on the Windows SDK/MSBuild
  toolchain being present on a packaging machine - useful for CI later.

This can be revisited if UNI 76 grows other Windows-specific installer needs
(e.g. driver-style components), but there's no reason to reintroduce that
complexity now.

## WebView2 Runtime

UNI 76's UI depends on the Microsoft Edge WebView2 Runtime being present on
the end-user's machine (the plugin itself statically links the *loader*,
not the runtime/browser engine - see [docs/BUILD.md](../../docs/BUILD.md)).

Most Windows 11 (and increasingly Windows 10) machines already have it,
since Windows and Edge install it as a shared system component. The future
production installer must not assume this, though. Before the first public
release, `UNI76.iss` needs to:

1. Check for the runtime (registry key documented as a TODO in the `.iss`
   file).
2. If missing, download and silently run Microsoft's small "Evergreen
   Bootstrapper" (`MicrosoftEdgeWebview2Setup.exe`) to install it.

This is **not implemented yet** - flagging it here so it isn't lost, and so
nobody mistakes the current skeleton for a runtime-safe installer.

## Code signing

Not addressed at this stage. A Windows Authenticode certificate will be
needed before public release so installers/binaries don't trigger
SmartScreen warnings. No certificates, passwords, or other secrets belong in
this repository - signing must happen via a secrets-managed CI step or a
signing machine, never by committing key material here.
