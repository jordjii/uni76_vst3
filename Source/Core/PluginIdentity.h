#pragma once

/*
    Centralised, non-DSP identity constants for UNI 76.

    Company/bundle/plugin-code identity lives in cmake/PluginIdentity.cmake
    (it must be visible to CMake). This header holds the C++-only constants
    that don't need to be known at configure time, such as the preset/state
    schema version used to migrate saved sessions in future releases.
*/

namespace uni76
{
    /** Bumped whenever the shape of the saved APVTS state changes in a way
        that requires migration code in PluginProcessor::setStateInformation.

        v1: the 7 APVTS parameters only.
        v2: added the 7 module-enabled flags (Core/ModuleEnableState.h) as
            plain properties alongside the parameters. A v1 state simply
            won't have these properties, and setStateInformation() already
            treats a missing flag as enabled=true, so no dedicated
            migration branch was needed for this bump - the version number
            is still incremented so the saved format is self-documenting.
        v3: `pitch` changed from a 0..100% AudioParameterFloat (no DSP
            behind it) to a discrete -12..+12 semitone AudioParameterInt
            (see docs/DSP_PITCH.md). A pre-v3 state's saved `pitch` value
            is a raw 0..100 number that never had any semitone meaning -
            setStateInformation() explicitly forces `pitch` to its new
            default (0 ST) whenever loadedSchemaVersion < 3, rather than
            letting APVTS reinterpret that old raw number as a semitone
            offset (which would silently clamp/misread it).
        v4: (superseded by v5 below - kept here for the log's honesty, not
            because a v4 state should still be treated specially.)
            `panorama`'s default changed from 0% to 50% under a first
            revision of PAN/STEREO FIELD DSP that used a MONO(0%)/
            NATURAL(50%)/WIDE(100%) product contract. That contract was
            retired before public release in favour of the ORIGINAL(0%)/
            WIDE(50%)/MOTION(100%) contract v5 describes - v4's 50%
            default and its "NATURAL" meaning no longer exist.
        v5: `panorama`'s default reverted to 0% under the current, final
            PAN contract - ORIGINAL(0%, bit-exact identity)/WIDE(50%)/
            MOTION(100%) (see docs/DSP_PAN.md). Any state saved with
            loadedSchemaVersion < 5 - whether a genuinely old pre-DSP
            state *or* a v4 state saved under the retired MONO/NATURAL/
            WIDE contract - has its `panorama` value forced to 0%
            (ORIGINAL), for the same reason PITCH's v3 migration forces
            old values to its new default: neither an old pre-DSP value
            nor a v4-era "50% NATURAL" choice has any meaning under the
            current contract, so there is nothing to "best-effort"
            preserve - forcing every pre-v5 state to ORIGINAL is what
            guarantees an old (or v4-only) project can't suddenly play
            altered after this update. This plugin has not had a public
            release yet (see CLAUDE.md), so there is no real installed
            base whose v4 "NATURAL" choices this migration could be
            accused of destroying - only local development states.
    */
    inline constexpr int stateSchemaVersion = 5;

    /** `imageTilt` (docs/DSP_IMAGE.md) was added as a brand-new APVTS
        parameter without bumping stateSchemaVersion, unlike PITCH's v3
        and PAN's v5 bumps above. Those two needed explicit migration
        code because an *existing* parameter ID's stored value stopped
        meaning what it used to mean - a state saved before the change
        still has a value for that ID, and letting APVTS reinterpret it
        under the new meaning would be silently wrong. `imageTilt` has no
        such old value to reinterpret: any state saved before this
        parameter existed simply has no ValueTree child for it at all,
        and juce::AudioProcessorValueTreeState::replaceState() already
        leaves a parameter at its constructed default (0, CENTER - see
        ParameterLayout.cpp's makeImageTiltParameter()) whenever the
        incoming state has no matching child, with no special-case code
        required - verified directly by
        Tests/PluginTests.cpp's "Legacy state without imageTilt defaults
        to 0 (CENTER)" test. Bumping the schema version for a genuinely
        new, independently-defaulting parameter would only be
        self-documentation for its own sake, not a functional need - see
        this constant's own doc comment above ("bumped whenever the
        shape of the saved state changes in a way that *requires
        migration code*").
    */

    /** Property name under which stateSchemaVersion is stored in the saved
        ValueTree, so setStateInformation can detect old presets.
    */
    inline constexpr const char* stateSchemaVersionProperty = "uni76StateSchemaVersion";

    /** The schema version at which `pitch` became a discrete -12..+12
        AudioParameterInt (see stateSchemaVersion's v3 entry above). Fixed
        at 3 regardless of any later bump to stateSchemaVersion - a future
        unrelated migration must not re-trigger the pitch-value-stripping
        branch in setStateInformation() for states that are already v3+.
    */
    inline constexpr int pitchDiscreteSchemaVersion = 3;

    /** The schema version at which `panorama` settled on its current,
        final ORIGINAL(0%)/WIDE(50%)/MOTION(100%) contract and 0% default
        (see stateSchemaVersion's v5 entry above - this also covers and
        supersedes the retired v4 MONO/NATURAL/WIDE contract). Fixed at 5
        regardless of any later bump to stateSchemaVersion - a future
        unrelated migration must not re-trigger the panorama-value-
        stripping branch in setStateInformation() for states that are
        already v5+.
    */
    inline constexpr int panoramaOriginalSchemaVersion = 5;

    /** Property names under which the active-preset identity (which
        preset, factory or user, is currently applied) is stored in the
        saved ValueTree - see PluginProcessor::setActivePresetInfo() and
        WebUIEditor.h's ActivePresetInfo. Tracking this on the *processor*
        (not just the editor) is what makes the displayed preset name
        survive closing and reopening the plugin editor window - before
        this, the name lived only on the editor instance, which JUCE
        destroys and recreates on every editor close/open, so the name
        silently reverted to "Default" even though the actual parameter
        values (the real preset content) were unaffected, since those
        alone were already part of the APVTS-backed saved state. */
    inline constexpr const char* activePresetKindProperty = "uni76ActivePresetKind";
    inline constexpr const char* activePresetNameProperty = "uni76ActivePresetName";
}
