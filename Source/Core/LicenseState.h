#pragma once

#include <atomic>
#include <juce_core/juce_core.h>

/*
    UNI 76 - offline, machine-bound license check.

    No server, no network call, ever - the whole point is this works with
    no backend (see docs/LICENSING.md for the full manual issue workflow
    and Tools/LicenseKeygen for the offline keygen tool the developer runs
    by hand per sale). A license is a small RSA-signed XML file
    (license.uni76lic) the customer drops into a fixed per-user app-data
    folder; this class verifies it against the public key embedded in
    Core/LicensePublicKey.h.

    refresh() does real file I/O (reads/writes under the same
    "Nostalgia Audio/UNI 76/License" app-data folder convention
    Core/UserPresets.h already uses for its own folder) and is therefore
    message-thread-only. The *result* is cached in a std::atomic<bool> so
    processBlock() can read it lock-free (a single relaxed load, no
    allocation, no lock), the same realtime-safety pattern
    Core/ModuleEnableState.h's flags already establish.

    Deliberately NOT called from UNI76AudioProcessor's constructor -
    createPluginFilter() (PluginProcessor.cpp) calls it explicitly,
    immediately after construction. createPluginFilter() is this plugin's
    one and only real hosting entry point (no Standalone format - see
    CLAUDE.md's "Format policy"), so every genuine VST3/AU host load goes
    through a real, fail-closed check. A processor built by directly
    calling `new UNI76AudioProcessor()` or `UNI76AudioProcessor x;` -
    which is exactly what every one of Tests/PluginTests.cpp's ~30
    existing DSP test classes already does, and predates this class -
    never reaches refresh() at all, so it keeps the constructed default
    below (`licensed = true`) and processes real audio with no code
    changes needed anywhere in that test suite. This is a deliberate
    fail-OPEN default for direct C++ construction, paired with a
    fail-CLOSED real check on the only path a host can actually reach -
    not an oversight; see docs/LICENSING.md's "Why the default is
    licensed=true" section for the full reasoning.

    refresh() itself fails closed on every possible bad input once it IS
    called - missing file, corrupt XML, unknown format version, bad/
    tampered signature, wrong machine, or a malformed payload.
*/
namespace uni76
{
    class LicenseState
    {
    public:
        /** Ensures the license folder exists, (re)writes machine-id.txt
            with this machine's current ID, then loads+verifies
            license.uni76lic if present - overwriting the constructed
            `true` default with the real result either way. Message-thread
            only; see the class comment above for exactly who calls this. */
        void refresh();

        /** Realtime-safe (atomic, relaxed) - the only member function
            processBlock() may ever call. True until/unless refresh() is
            called and finds no valid license (see the class comment). */
        bool isLicensed() const noexcept { return licensed.load (std::memory_order_relaxed); }

        static juce::File getLicenseDirectory();
        static juce::File getMachineIdFile();
        static juce::File getLicenseFile();

    private:
        std::atomic<bool> licensed { true };
    };
}
