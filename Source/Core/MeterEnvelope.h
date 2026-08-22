#pragma once

/*
    Message-thread-only envelope follower for the INPUT/OUTPUT meter
    telemetry: fast attack, slow release, so the meter reads as a smooth
    analogue needle rather than a flickering per-block value.

    Pulled out into its own header (rather than living as a private detail
    of Source/UI/WebUIEditor.cpp) purely so Tests/PluginTests.cpp can
    exercise the actual release-to-silence behaviour directly, without
    needing to construct a WebView editor to do it.
*/

namespace uni76
{
    inline constexpr float meterAttackCoeff  = 0.6f;
    inline constexpr float meterReleaseCoeff = 0.08f;

    inline float applyMeterEnvelope (float previous, float target) noexcept
    {
        const auto coeff = target > previous ? meterAttackCoeff : meterReleaseCoeff;
        return previous + (target - previous) * coeff;
    }
}
