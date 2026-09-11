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
      4 = panorama, 5 = reverb, 6 = imager.

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
        static constexpr int numModules = 7;

        /** Which module role runs at chain position `position` (0..6,
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

        /** Resets to the original, factory PREAMP->EQ->SAT->PITCH->PAN->
            VERB->IMAGE order - the identity permutation {0,1,2,3,4,5,6}. */
        void resetToDefault() noexcept
        {
            setOrder ({ 0, 1, 2, 3, 4, 5, 6 });
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
            ValueTree - a single comma-joined string ("0,1,2,3,4,5,6"),
            same spirit as ModuleEnableState's per-flag properties but one
            property is simpler here since order is inherently one unit,
            not 7 independent booleans. */
        static constexpr const char* stateProperty = "chainOrder";

    private:
        std::array<std::atomic<int>, numModules> order { { 0, 1, 2, 3, 4, 5, 6 } };
    };
}
