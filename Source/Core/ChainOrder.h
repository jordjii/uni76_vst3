#pragma once

#include <array>
#include <atomic>

/*
    UNI 76 - user-reorderable signal chain (drag-and-drop pedalboard
    round).

    The 7 modules' *processing order* is now user-controllable, not a
    fixed PREAMP->EQ->SAT->PITCH->PAN->VERB->IMAGE sequence - dragging a
    module's panel in the UI genuinely changes which order
    PluginProcessor::processBlock() calls each `xxxProcessor.process()` in,
    so the sound changes with the reorder, not just the on-screen layout.

    Deliberately NOT an APVTS parameter (same reasoning as
    ModuleEnableState.h): it's structural/session state, not something a
    host should offer as an automatable lane. Persisted as a plain
    property on the same saved ValueTree as everything else (see
    PluginProcessor::getStateInformation/setStateInformation), same
    pattern the module-enabled flags already use.

    `order[position]` holds which module *role* runs at that position in
    the chain - role indices match ModuleEnableState::propertyNames'
    order (and Resources/Web/module_power.js's MODULE_ORDER), so the two
    files never need a third, independent index scheme:
      0 = preamp, 1 = eq, 2 = saturation, 3 = pitch,
      4 = panorama, 5 = reverb, 6 = imager, 7 = delay.

    Role 7 (DELAY) was added 2026-09-14 (see docs/DSP_DELAY.md) - an
    APPENDED role index, not a renumbering of any existing role, same
    "append, never renumber" precedent every parameter added after the
    original 7 already established. Its new DEFAULT chain position sits
    between PAN and VERB (see the default order below) even though its
    role *index* is the highest number - position and role index are
    independent: the default array below is what actually encodes
    "DELAY runs 6th", not role 7's own numeric value.

    Total plugin latency is unaffected by reordering - it is the *sum* of
    each module's own latency (see PluginProcessor::updateReportedLatency())
    and addition is order-independent, so no latency-recompute call is
    needed anywhere ChainOrder changes.
*/

namespace uni76
{
    class ChainOrder
    {
    public:
        static constexpr int numModules = 8;

        /** Which module role runs at chain position `position` (0..7,
            position 0 runs first). Out-of-range reads back an identity
            mapping rather than garbage, matching ModuleEnableState's own
            defensive-read convention. */
        int roleAtPosition (int position) const noexcept
        {
            return isValidPosition (position) ? order[(size_t) position].load (std::memory_order_relaxed) : position;
        }

        std::array<int, numModules> snapshot() const noexcept
        {
            std::array<int, numModules> result {};
            for (int i = 0; i < numModules; ++i)
                result[(size_t) i] = order[(size_t) i].load (std::memory_order_relaxed);
            return result;
        }

        /** Replaces the whole order at once - the only way it's ever
            changed (a drag-and-drop reorder always produces a complete new
            permutation, never a single-slot edit). Caller must have
            already validated `newOrder` with isValidPermutation() - this
            does not re-check, to keep it callable from a realtime-safe
            context if that's ever needed (it isn't today; this is only
            ever called from the message thread). */
        void setOrder (const std::array<int, numModules>& newOrder) noexcept
        {
            for (int i = 0; i < numModules; ++i)
                order[(size_t) i].store (newOrder[(size_t) i], std::memory_order_relaxed);
        }

        /** Resets to the current factory PREAMP->EQ->SAT->PITCH->PAN->
            DELAY->VERB->IMAGE order - {0,1,2,3,4,7,5,6} (role 7/DELAY
            sits at position 5, between PAN and VERB - see the class
            comment above for why role *index* and chain *position* are
            independent numbers). */
        void resetToDefault() noexcept
        {
            setOrder ({ 0, 1, 2, 3, 4, 7, 5, 6 });
        }

        static bool isValidPosition (int position) noexcept
        {
            return position >= 0 && position < numModules;
        }

        /** A genuine reorder must be a permutation of every role exactly
            once - never fewer/more/duplicate roles. Used both to validate
            a UI-originated reorder before applying it and to reject a
            corrupt/hand-edited saved state (falling back to the default
            order rather than crashing or silently duplicating a module's
            processing). */
        static bool isValidPermutation (const std::array<int, numModules>& candidate) noexcept
        {
            std::array<bool, numModules> seen {};
            for (auto role : candidate)
            {
                if (role < 0 || role >= numModules || seen[(size_t) role])
                    return false;
                seen[(size_t) role] = true;
            }
            return true;
        }

        /** Property name used to persist the order on the saved state
            ValueTree - a single comma-joined string
            ("0,1,2,3,4,7,5,6"), same spirit as ModuleEnableState's
            per-flag properties but one property is simpler here since
            order is inherently one unit, not 8 independent booleans. */
        static constexpr const char* stateProperty = "chainOrder";

        /** A saved ChainOrder from before DELAY existed has exactly 7
            tokens (the old numModules) and never mentions role 7 at all.
            Per the product brief's own explicit backward-compatibility
            rule: insert DELAY safely before VERB (role 5) without
            disturbing the relative order of anything else, rather than
            falling back to the full default order (which would also
            silently discard a user's own custom reorder of the other 6
            modules). Returns the identity default if `oldOrder` isn't
            itself a valid 7-role permutation (defensive - a corrupt/
            hand-edited old state shouldn't propagate into an equally
            invalid 8-role array). */
        static std::array<int, numModules> insertDelayIntoLegacyOrder (const std::array<int, 7>& oldOrder) noexcept
        {
            std::array<bool, 7> seen {};
            for (auto role : oldOrder)
                if (role < 0 || role >= 7 || seen[(size_t) role])
                    return { 0, 1, 2, 3, 4, 7, 5, 6 };
                else
                    seen[(size_t) role] = true;

            std::array<int, numModules> result {};
            int writeIndex = 0;
            for (auto role : oldOrder)
            {
                if (role == 5) // VERB - insert DELAY immediately before it
                    result[(size_t) writeIndex++] = 7;
                result[(size_t) writeIndex++] = role;
            }
            return result;
        }

    private:
        std::array<std::atomic<int>, numModules> order { { 0, 1, 2, 3, 4, 7, 5, 6 } };
    };
}
