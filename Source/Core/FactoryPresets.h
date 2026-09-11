#pragma once

#include <array>

/*
    UNI 76 - factory preset bank.

    Deliberately NOT a new saved-state format and NOT a new APVTS
    parameter: a preset is just a named set of *values* for the existing
    8 stable parameters (see Parameters/ParameterIDs.h - IDs are never
    renamed here) plus the 7 module-enabled flags (ModuleEnableState.h).
    Selecting a preset calls the exact same `setValueNotifyingHost()` /
    `ModuleEnableState::setEnabled()` paths a user turning a knob or
    clicking a power button already exercises - so it needs no new
    migration logic and is automatically captured by the existing
    getStateInformation()/setStateInformation() the next time the host
    saves, with no changes to that mechanism at all.

    Values are the parameters' own real (denormalised) units - the same
    units ParameterLayout.cpp declares each range in - not normalised
    0..1, so this table reads directly against docs/DSP_*.md's own
    percent/semitone/tilt language. Source/UI/WebUIEditor.cpp converts
    each value with that parameter's own `convertTo0to1()` before calling
    setValueNotifyingHost().

    `modulesEnabled` (live-testing follow-up round - not every module
    used to be enabled in every preset) - each preset now enables only
    the modules that genuinely contribute to its own sound, following
    two explicit rules:
      - PREAMP/SAT/PAN/VERB/IMAGE are enabled exactly when that module's
        own value for this preset is non-zero - a module sitting at 0%
        isn't "used" by this preset's own recipe.
      - EQ is enabled *only* on presets whose whole point is the always-
        on band-limited "telephone"/lo-fi character it now has (see
        docs/DSP_EQ.md's "Redesign" section) - "Telephone Plate" and
        "Lo-Fi Vocal" - never as a blanket default, since EQ's own centre
        position is no longer a transparent resting state.
      - PITCH is disabled everywhere: every ordinary instrument preset
        keeps it at 0 ST (no natural "vintage character" default exists
        for a pitch shift), so it never actually contributes anything a
        user hasn't dialled in themselves.
    "Default" therefore has every module disabled - a genuine, literal
    pass-through starting point, not just parameter values that happen to
    be near-identity.

    TILT stays at 0/CENTER unless a preset has a genuine artistic reason
    to lean the stereo image (documented per-preset below where used). No
    preset reaches 100% on any control - these are musical starting
    points, not the extreme-matrix stress combinations
    docs/FULL_DSP_AUDIT.md's own audit already covers separately.
*/

namespace uni76
{
    enum class PresetCategory
    {
        general,
        vocal,
        piano,
        acousticGuitar,
        electricGuitar
    };

    inline const char* presetCategoryName (PresetCategory category) noexcept
    {
        switch (category)
        {
            case PresetCategory::general:        return "GENERAL";
            case PresetCategory::vocal:           return "VOCAL";
            case PresetCategory::piano:           return "PIANO";
            case PresetCategory::acousticGuitar:  return "ACOUSTIC GUITAR";
            case PresetCategory::electricGuitar:  return "ELECTRIC GUITAR";
        }
        return "GENERAL";
    }

    struct FactoryPreset
    {
        const char* name;
        PresetCategory category;

        // Real units, in ParamID::all order: preamp%, eq%, saturation%,
        // pitchST, panorama%, reverb%, imager%, imageTilt.
        float preamp;
        float eq;
        float saturation;
        float pitch;
        float panorama;
        float reverb;
        float imager;
        float imageTilt;

        // Module-enabled flags, in ModuleEnableState.h's own order:
        // preamp, eq, saturation, pitch, panorama, reverb, imager.
        std::array<bool, 7> modulesEnabled;
    };

    /*
        Rebalance pass (live-testing follow-up, same round as the
        drive-curve/output-trim fix in PreampCurves.h/SatCurves.h - see
        CLAUDE.md). Direct feedback: too many presets sat close enough to
        their modules' own off/neutral position that they read as barely
        doing anything.

        PREAMP/SAT's own minimums were already reasonably audible once the
        drive-curve fix landed (DRIVE/HEAT=25% now measures ~1.7% THD, not
        the near-silent figure the old back-loaded curve gave it), so those
        two columns only needed a moderate lift. PAN, VERB and IMAGE were
        the real offenders: none of their curves were touched this round,
        and each has a "quiet" region low values sit inside by design -
        PAN's own width mapping is close to 1.0x (no audible width) below
        roughly 25%, VERB's send is a light dusting anywhere under ~20%,
        IMAGE's shelf asymptotes barely move off 1.0x under ~15%. Presets
        that had these at 5-20% were, by the modules' own measured curves,
        close to inaudible on exactly the axis their name promised ("Wide
        Grand" at panorama=40%/imager=35% is a defensible middle value, but
        "Focused Stereo" at panorama=15%/verb=10% and "Vintage Vocal" at
        panorama=5%/imager=5% were not). Every "wide"/"motion"/"backing"
        preset now reaches PAN's own WIDE(50%) region or beyond; every
        "plate"/"deep" preset reaches a send strong enough to be
        unmistakably a plate, not a hint of one; every other preset's
        PAN/VERB/IMAGE floor was raised out of the sub-20% dead zone.
        "Telephone Plate"'s own `eq` value was also moved from 55% (a mild
        AIR lean) to 35% (toward DARK), since the redesigned EQ's DARK
        direction is what actually narrows the passband into a boxier,
        more literally "telephone" character - see docs/DSP_EQ.md's anchor
        table; PHONE/50% (this module's own resting default) is already
        the midpoint, not the extreme.

        Still no preset at 100% on any control, still musical starting
        points rather than the extreme-matrix stress combinations
        docs/FULL_DSP_AUDIT.md covers separately, still PITCH=0/TILT=0
        unless documented per-preset.
    */
    inline constexpr std::array<FactoryPreset, 32> factoryPresets { {
        // ---- GENERAL ------------------------------------------------------
        // Name              Category                     preamp  eq    sat   pitch  pan   verb  imager tilt   { preamp, eq,    sat,   pitch, pan,   verb,  imager }
        { "Default",          PresetCategory::general,        0.0f, 50.0f,  0.0f, 0.0f,  0.0f,  0.0f,  0.0f, 0.0f, { false, false, false, false, false, false, false } },
        { "Warm Analog",      PresetCategory::general,       30.0f, 40.0f, 22.0f, 0.0f, 32.0f, 26.0f, 20.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Dark Vintage",     PresetCategory::general,       38.0f, 20.0f, 32.0f, 0.0f, 22.0f, 26.0f, 14.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Telephone Plate",  PresetCategory::general,       34.0f, 35.0f, 26.0f, 0.0f, 26.0f, 55.0f, 18.0f, 0.0f, { true,  true,  true,  false, true,  true,  true  } },
        { "Wide Vintage",     PresetCategory::general,       26.0f, 45.0f, 20.0f, 0.0f, 58.0f, 30.0f, 50.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Motion Space",     PresetCategory::general,       16.0f, 50.0f, 10.0f, 0.0f, 80.0f, 42.0f, 40.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Focused Stereo",   PresetCategory::general,       20.0f, 50.0f, 14.0f, 0.0f, 28.0f, 18.0f, 32.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Deep Plate",       PresetCategory::general,       20.0f, 45.0f, 14.0f, 0.0f, 26.0f, 85.0f, 20.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Hot Console",      PresetCategory::general,       60.0f, 60.0f, 52.0f, 0.0f, 22.0f, 14.0f, 18.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Clean Wide",       PresetCategory::general,       12.0f, 55.0f,  0.0f, 0.0f, 55.0f, 18.0f, 45.0f, 0.0f, { true,  false, false, false, true,  true,  true  } },

        // ---- VOCAL ------------------------------------------------------
        // Gentle drive (vocals distort fast), EQ leaning AIR for presence,
        // modest width (vocals usually stay fairly centred), PLATE-range
        // VERB for the "sung into a room" cue.
        { "Warm Lead",        PresetCategory::vocal,          26.0f, 45.0f, 16.0f, 0.0f, 22.0f, 24.0f, 18.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Airy Lead",        PresetCategory::vocal,          16.0f, 70.0f, 10.0f, 0.0f, 22.0f, 30.0f, 26.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Vintage Vocal",    PresetCategory::vocal,          34.0f, 30.0f, 24.0f, 0.0f, 16.0f, 22.0f, 14.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Plate Vocal",      PresetCategory::vocal,          20.0f, 55.0f, 14.0f, 0.0f, 20.0f, 50.0f, 18.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Wide Backing",     PresetCategory::vocal,          14.0f, 50.0f,  8.0f, 0.0f, 60.0f, 34.0f, 50.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Lo-Fi Vocal",      PresetCategory::vocal,          44.0f, 12.0f, 40.0f, 0.0f,  0.0f, 14.0f,  0.0f, 0.0f, { true,  true,  true,  false, false, true,  false } },

        // ---- PIANO --------------------------------------------------------
        // Piano has a very wide natural frequency range - keep PREAMP/SAT
        // low (avoid muddying the bass end), use IMAGE/VERB to shape width
        // and space instead. EQ stays off - its always-on telephone-band
        // character is never appropriate for piano's own wide range.
        { "Warm Upright",     PresetCategory::piano,          20.0f, 35.0f, 14.0f, 0.0f, 24.0f, 26.0f, 18.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Focused Grand",    PresetCategory::piano,          14.0f, 50.0f,  8.0f, 0.0f, 24.0f, 20.0f, 22.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Wide Grand",       PresetCategory::piano,          14.0f, 50.0f,  8.0f, 0.0f, 58.0f, 26.0f, 50.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Vintage Piano",    PresetCategory::piano,          30.0f, 25.0f, 20.0f, 0.0f, 22.0f, 20.0f, 14.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Deep Plate Piano", PresetCategory::piano,          14.0f, 45.0f,  8.0f, 0.0f, 26.0f, 62.0f, 20.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },

        // ---- ACOUSTIC GUITAR -----------------------------------------------
        // Mid-driven instrument - moderate SAT for string "grit", IMAGE up
        // a bit more freely than piano/vocal since it sits well off-centre
        // in a mix. EQ stays off here too - none of these are meant to
        // sound band-limited/telephone.
        { "Warm Fingerstyle", PresetCategory::acousticGuitar, 26.0f, 40.0f, 20.0f, 0.0f, 22.0f, 22.0f, 20.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Bright Strum",     PresetCategory::acousticGuitar, 14.0f, 65.0f, 14.0f, 0.0f, 28.0f, 20.0f, 26.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Vintage Wood",     PresetCategory::acousticGuitar, 36.0f, 25.0f, 30.0f, 0.0f, 18.0f, 20.0f, 16.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Wide Acoustic",    PresetCategory::acousticGuitar, 18.0f, 50.0f, 14.0f, 0.0f, 58.0f, 26.0f, 48.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Plate Acoustic",   PresetCategory::acousticGuitar, 18.0f, 45.0f, 14.0f, 0.0f, 26.0f, 52.0f, 20.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },

        // ---- ELECTRIC GUITAR -------------------------------------------
        // The one instrument family where real HEAT/DRIVE is idiomatic -
        // still nowhere near 100% (a genuinely overdriven amp is the
        // guitar's own job, not this insert's). EQ stays off - none of
        // these lean into a telephone/lo-fi character either.
        { "Clean Console",    PresetCategory::electricGuitar, 26.0f, 45.0f, 14.0f, 0.0f, 18.0f, 14.0f, 16.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Warm Rhythm",      PresetCategory::electricGuitar, 36.0f, 35.0f, 30.0f, 0.0f, 20.0f, 14.0f, 14.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Vintage Lead",     PresetCategory::electricGuitar, 46.0f, 30.0f, 40.0f, 0.0f, 16.0f, 24.0f, 12.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Wide Clean",       PresetCategory::electricGuitar, 20.0f, 50.0f, 10.0f, 0.0f, 58.0f, 20.0f, 46.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Plate Lead",       PresetCategory::electricGuitar, 36.0f, 40.0f, 30.0f, 0.0f, 20.0f, 48.0f, 16.0f, 0.0f, { true,  false, true,  false, true,  true,  true  } },
        { "Dark Rhythm",      PresetCategory::electricGuitar, 40.0f, 15.0f, 34.0f, 0.0f, 10.0f, 14.0f,  0.0f, 0.0f, { true,  false, true,  false, true,  true,  false } },
    } };
}
