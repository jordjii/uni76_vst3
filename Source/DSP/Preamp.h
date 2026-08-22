#pragma once

namespace uni76::dsp
{
    /*
        Architectural placeholder for the future PREAMP module.

        This will eventually own the plugin's input gain stage modelling and
        the internal (non-automatable) Low Cut / High Cut shaping. Nothing is
        implemented yet - this stage of the project is foundation only, no
        DSP is written or wired into the signal path.
    */
    class Preamp
    {
    public:
        Preamp() = default;
    };
}
