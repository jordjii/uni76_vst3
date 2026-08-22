#pragma once

#include <array>
#include <atomic>

/*
    Persistent, but NOT DAW-automatable, per-module enabled/disabled flags.

    These are deliberately not APVTS RangedAudioParameters: UNI 76's public
    automation surface is fixed at exactly 7 parameters (see CLAUDE.md).
    This is an internal on/off switch the UI and (once it exists) each
    module's DSP read - not something a host should see as an automation
    lane. It still has to survive editor close/reopen and host state
    save/reload, so it's persisted as plain properties on the same saved
    ValueTree as everything else (see PluginProcessor::getStateInformation),
    just outside the APVTS parameter tree itself.

    Stored as atomics (not because the audio thread reads them yet - it
    doesn't, since there's no DSP to bypass) so that when a module's DSP
    is eventually wired in, `if (! moduleEnableState.isEnabled (i))` can be
    read straight from processBlock() with no extra synchronisation work.
*/

namespace uni76
{
    class ModuleEnableState
    {
    public:
        static constexpr int numModules = 7;

        bool isEnabled (int index) const noexcept
        {
            return isValidIndex (index) ? flags[(size_t) index].load (std::memory_order_relaxed) : true;
        }

        void setEnabled (int index, bool enabled) noexcept
        {
            if (isValidIndex (index))
                flags[(size_t) index].store (enabled, std::memory_order_relaxed);
        }

        static bool isValidIndex (int index) noexcept
        {
            return index >= 0 && index < numModules;
        }

        /** Property names used to persist each flag on the saved state
            ValueTree - order matches Source/Parameters/ParameterIDs.h's
            ParamID::all (preamp, eq, saturation, pitch, panorama, reverb,
            imager), which is also the order the frontend uses.
        */
        static constexpr std::array<const char*, numModules> propertyNames
        {
            "preampEnabled", "eqEnabled", "saturationEnabled", "pitchEnabled",
            "panoramaEnabled", "reverbEnabled", "imagerEnabled"
        };

    private:
        std::array<std::atomic<bool>, numModules> flags { { true, true, true, true, true, true, true } };
    };
}
