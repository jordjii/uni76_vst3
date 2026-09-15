# UNI 76 - offline license / copy-protection

This is the manual, no-backend workflow for issuing a license to a
customer, and the reasoning behind the mechanism. See
`Source/Core/LicenseState.h`, `Source/Core/LicenseCrypto.h`,
`Source/Core/LicensePublicKey.h` and `Tools/LicenseKeygen/main.cpp` for
the implementation; CLAUDE.md's journal has the dated entry for when this
shipped.

## Why this shape

UNI 76 ships bundled with a paid sound pack. There is no website/backend
for license issuance today, so the whole system works with **no network
call, ever** - the plugin never phones home, and the developer's own
issuing process is a local command-line tool run by hand per sale.

Realistic goal, stated plainly: this stops *casual* leaking effectively -
a bare `.vst3`/`.component` file is useless without a machine-bound signed
license file only the developer can issue, and forging one requires
breaking RSA-3072, not just copying a file. It does **not** stop a
determined attacker from patching the compiled plugin binary to skip the
check entirely (a one-branch patch in `processBlock()`) - no offline DRM
can prevent that, and this project isn't pretending otherwise. If this
plugin becomes popular enough to attract that kind of attention, revisit
with actual obfuscation/anti-tamper measures at that point - not before.

## One-time setup (do this once, ever)

1. Build the keygen tool (never part of a normal plugin build - `OFF` by
   default):
   ```
   cmake --preset windows-release -DUNI76_BUILD_TOOLS=ON
   cmake --build build/windows-release --config Release --target UNI76LicenseKeygen
   ```
2. Generate a keypair:
   ```
   build/windows-release/Tools/LicenseKeygen/Release/UNI76LicenseKeygen.exe genkeys
   ```
   This writes `keys/public.key.txt` and `keys/private.key.txt` in the
   current directory (gitignored - see `Tools/LicenseKeygen/keys/` in
   `.gitignore`).
3. Paste the printed public key into
   `Source/Core/LicensePublicKey.h`'s `licensePublicKeyString`, then
   rebuild and ship the plugin. **Never commit, email, or otherwise share
   `private.key.txt`** - it is the only thing that can issue a valid
   license, kept only on the developer's own machine (back it up somewhere
   private and durable; losing it means every future sale needs a new
   keypair, and old customers' licenses stay valid against the old public
   key baked into whatever binaries were already shipped).

## Per sale (do this every time)

1. Customer installs the plugin, opens their DAW once, then opens
   `machine-id.txt` (see below) and sends the ID inside, plus their
   purchase confirmation, to support.
2. Issue their license (repeat `--machine2` for a second computer if
   they've bought a 2-machine license; omit it for one machine):
   ```
   UNI76LicenseKeygen issue --email customer@example.com --machine1 THEIR-ID --machine2 THEIR-SECOND-ID --out customer-name.uni76lic
   ```
3. Email `customer-name.uni76lic` back. They rename it to
   `license.uni76lic` (or just drop it in - the tool already names the
   *contents* correctly, only the customer-facing filename is
   personalised for your own bookkeeping) into the same folder
   `machine-id.txt` was in, and restart their DAW.

## Where the files live

Same per-user app-data convention `Core/UserPresets.h` already uses for
its own folder:

- Windows: `%APPDATA%\Nostalgia Audio\UNI 76\License\`
- macOS: `~/Library/Application Support/Nostalgia Audio/UNI 76/License/`

Two files:
- `machine-id.txt` - human-readable, (re)written by the plugin on every
  real host load. Never touch this yourself; it's just a courier for
  `juce::SystemStats::getUniqueDeviceID()`.
- `license.uni76lic` - the signed license file, dropped in by the
  customer. XML, one root element, three attributes (`version`, `payload`,
  `signature`). `payload` is `"UNI76|<formatVersion>|<email>|<machineId1>|
  <machineId2>|<issuedDateISO>"` (machineId2 may be empty for a
  single-machine license); `signature` is that payload's SHA-256 hash,
  RSA-signed with the developer's private key (see LicenseCrypto.h).

## How verification works

`UNI76AudioProcessor::refreshLicenseState()` - called exactly once, from
`createPluginFilter()` (the plugin's one real hosting entry point; there
is no Standalone format) - loads `license.uni76lic`, verifies its
signature against the public key embedded in `LicensePublicKey.h`, checks
the payload's product token and format version, and checks the current
machine's ID against the payload's one or two machine IDs. Any failure at
any step - missing file, corrupt XML, unknown version, bad/tampered
signature, wrong machine - leaves the plugin unlicensed. The result is
cached in a `std::atomic<bool>` (`Core/LicenseState.h`) so
`processBlock()` can check it with a single lock-free read; an unlicensed
instance outputs genuine silence (`buffer.clear()`), not a degraded
signal.

### Why the default is licensed=true

`LicenseState`'s cached flag defaults to `true`, and the real check only
ever runs when something explicitly calls `refresh()`. This is
deliberate: `createPluginFilter()` always calls it (so every real VST3/AU
host load gets the real, fail-closed check), but a `UNI76AudioProcessor`
built by directly constructing it in C++ - which is exactly what every
existing DSP test in `Tests/PluginTests.cpp` already does, and predates
this feature - never reaches `refresh()` at all, so those ~30 test
classes keep processing real audio with zero changes needed. Fail-open
for direct construction, fail-closed for every path an actual host can
reach.

## Machine limit enforcement is manual, not dynamic

Because there is no server, the "N machines per license" limit is
enforced by the developer choosing which machine IDs to bake into a
license at issue time - not tracked or capped automatically. Nothing
stops a customer from asking for a fresh license repeatedly, but doing so
requires a real email exchange each time, which is the actual friction
this system is meant to add. If a real backend/storefront exists in the
future, `LicenseCrypto.h`'s sign/verify functions and `LicenseState`'s
XML format don't need to change - only *how* a `.uni76lic` file reaches
the customer would move from "email" to "automated download," which is a
separate, later decision.
