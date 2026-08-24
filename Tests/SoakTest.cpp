// UNI 76 - RC1 blocker 3: accelerated offline long-run soak test.
//
// Not part of the regular UNI76Tests suite (kept as a separate, deliberately
// slow executable - see Tests/CMakeLists.txt) since it processes hours of
// equivalent audio and would otherwise make every routine test run
// multi-minute. Run on demand for release-readiness verification; results
// are recorded in docs/FULL_DSP_AUDIT.md, not gated in CI.

#include "Plugin/PluginProcessor.h"
#include "Parameters/ParameterIDs.h"

#include <windows.h>
#include <psapi.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>

namespace uni76soak
{
    std::atomic<long long> allocCount { 0 };
    std::atomic<bool> trackingAllocs { false };
}

void* operator new (std::size_t size)
{
    if (uni76soak::trackingAllocs.load (std::memory_order_relaxed))
        uni76soak::allocCount.fetch_add (1, std::memory_order_relaxed);
    void* p = std::malloc (size == 0 ? 1 : size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void* operator new[] (std::size_t size) { return operator new (size); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

namespace
{
    size_t currentWorkingSetBytes()
    {
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo (GetCurrentProcess(), &pmc, sizeof (pmc)))
            return pmc.WorkingSetSize;
        return 0;
    }

    // FNV-1a over the raw float bytes of every processed sample - a cheap,
    // deterministic whole-run checksum for the "same schedule twice ->
    // identical output" check.
    struct Fnv1a
    {
        std::uint64_t hash = 14695981039346656037ULL;
        void add (float v)
        {
            std::uint32_t bits;
            std::memcpy (&bits, &v, sizeof (bits));
            for (int i = 0; i < 4; ++i)
            {
                hash ^= (bits >> (i * 8)) & 0xFF;
                hash *= 1099511628211ULL;
            }
        }
    };

    struct SoakResult
    {
        long long samplesProcessed = 0;
        int blocksProcessed = 0;
        bool allFinite = true;
        double peak = 0.0;
        double dcSum = 0.0;
        std::uint64_t outputChecksum = 0;
        size_t memBeforeBytes = 0, memAfterBytes = 0, memPeakSampledBytes = 0;
        long long allocationsAfterPrepare = 0;
        double wallClockSeconds = 0.0;
    };

    // Deterministic per-block automation schedule - every one of the 8
    // parameters cycles through its own range on a different period, module
    // enables toggle on a coarser deterministic schedule. No randomness in
    // the *schedule* itself (juce::Random for the audio content only, fixed
    // seed) so two runs of runSoak() with the same arguments are bit-for-bit
    // comparable.
    SoakResult runSoak (double sampleRate, int blockSize, long long targetSamples, const char* label)
    {
        SoakResult result;
        UNI76AudioProcessor processor;
        processor.setBusesLayout ({ { juce::AudioChannelSet::stereo() }, { juce::AudioChannelSet::stereo() } });
        processor.prepareToPlay (sampleRate, blockSize);
        auto& apvts = processor.getValueTreeState();

        const char* paramIds[] {
            uni76::ParamID::preamp, uni76::ParamID::eq, uni76::ParamID::saturation, uni76::ParamID::pitch,
            uni76::ParamID::panorama, uni76::ParamID::reverb, uni76::ParamID::imager, uni76::ParamID::imageTilt
        };

        juce::Random random (12345);
        Fnv1a checksum;

        result.memBeforeBytes = currentWorkingSetBytes();
        result.memPeakSampledBytes = result.memBeforeBytes;

        const auto startTime = std::chrono::steady_clock::now();

        int block = 0;
        long long done = 0;
        bool trackingAllocsStarted = false;

        while (done < targetSamples)
        {
            for (int pi = 0; pi < 8; ++pi)
            {
                const auto phase = std::sin (2.0 * juce::MathConstants<double>::pi * (double) block / (double) (37 + pi * 11));
                if (auto* p = apvts.getParameter (paramIds[pi]))
                    p->setValueNotifyingHost ((float) (0.5 + 0.5 * phase));
            }

            if (block % 401 == 0)
                for (int m = 0; m < uni76::ModuleEnableState::numModules; ++m)
                    processor.getModuleEnableState().setEnabled (m, ((block / 401) + m) % 3 != 0);

            const auto thisBlock = (int) juce::jmin ((long long) blockSize, targetSamples - done);
            juce::AudioBuffer<float> buffer (2, thisBlock);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < thisBlock; ++i)
                    buffer.setSample (ch, i, random.nextFloat() * 0.6f - 0.3f);

            // Start allocation tracking only after the first block (mirrors
            // Tests/PluginTests.cpp's own allocation-audit test - prepare()
            // is where one-time setup allocates, not processBlock()).
            if (! trackingAllocsStarted && block == 1)
            {
                uni76soak::trackingAllocs.store (true);
                trackingAllocsStarted = true;
            }

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);

            for (int ch = 0; ch < 2; ++ch)
            {
                for (int i = 0; i < thisBlock; ++i)
                {
                    const auto s = buffer.getSample (ch, i);
                    if (! std::isfinite (s))
                    {
                        result.allFinite = false;
                        std::cout << "[" << label << "] NON-FINITE at block " << block << " ch " << ch << " sample " << i << std::endl;
                    }
                    result.peak = juce::jmax (result.peak, (double) std::abs (s));
                    result.dcSum += (double) s;
                    checksum.add (s);
                }
            }

            done += thisBlock;
            ++block;

            if (block % 20000 == 0)
            {
                const auto mem = currentWorkingSetBytes();
                result.memPeakSampledBytes = juce::jmax (result.memPeakSampledBytes, mem);
                std::cout << "[" << label << "] progress: " << done << "/" << targetSamples
                           << " samples, workingSet=" << (mem / (1024 * 1024)) << "MB" << std::endl;
            }
        }

        uni76soak::trackingAllocs.store (false);
        result.allocationsAfterPrepare = uni76soak::allocCount.load();

        const auto endTime = std::chrono::steady_clock::now();
        result.wallClockSeconds = std::chrono::duration<double> (endTime - startTime).count();

        result.samplesProcessed = done;
        result.blocksProcessed = block;
        result.outputChecksum = checksum.hash;
        result.memAfterBytes = currentWorkingSetBytes();
        result.memPeakSampledBytes = juce::jmax (result.memPeakSampledBytes, result.memAfterBytes);

        return result;
    }

    void printResult (const SoakResult& r, const char* label, double sampleRate)
    {
        const auto equivalentSeconds = (double) r.samplesProcessed / sampleRate;
        std::cout << "\n=== " << label << " ===" << std::endl;
        std::cout << "  samplesProcessed=" << r.samplesProcessed << " blocks=" << r.blocksProcessed
                   << " equivalentAudio=" << (equivalentSeconds / 60.0) << "min"
                   << " wallClock=" << r.wallClockSeconds << "s"
                   << " realtimeRatio=" << (r.wallClockSeconds / equivalentSeconds) << std::endl;
        std::cout << "  allFinite=" << (r.allFinite ? "true" : "false")
                   << " peak=" << r.peak << " meanDC=" << (r.dcSum / (double) (r.samplesProcessed * 2))
                   << " outputChecksum=0x" << std::hex << r.outputChecksum << std::dec << std::endl;
        std::cout << "  memBefore=" << (r.memBeforeBytes / (1024 * 1024)) << "MB"
                   << " memAfter=" << (r.memAfterBytes / (1024 * 1024)) << "MB"
                   << " memPeakSampled=" << (r.memPeakSampledBytes / (1024 * 1024)) << "MB"
                   << " memGrowth=" << ((double) ((long long) r.memAfterBytes - (long long) r.memBeforeBytes) / (1024.0 * 1024.0)) << "MB"
                   << " allocationsAfterPrepare=" << r.allocationsAfterPrepare << std::endl;
    }
}

int main()
{
    std::cout << "UNI 76 RC1 blocker 3: accelerated offline soak test" << std::endl;

    // Required minimum: 60 min-equivalent @ 48kHz stereo, plus a shorter
    // pass at 96kHz. Block size 512 throughout (representative host size).
    constexpr double sr48 = 48000.0;
    constexpr double sr96 = 96000.0;
    constexpr int blockSize = 512;

    const long long samples60MinAt48k = (long long) (60.0 * 60.0 * sr48);
    const long long samples20MinAt96k = (long long) (20.0 * 60.0 * sr96);

    auto run48kA = runSoak (sr48, blockSize, samples60MinAt48k, "48kHz run A");
    printResult (run48kA, "48kHz run A (60min-equivalent)", sr48);

    auto run48kB = runSoak (sr48, blockSize, samples60MinAt48k, "48kHz run B (repeat)");
    printResult (run48kB, "48kHz run B (repeat, determinism check)", sr48);

    std::cout << "\n=== Determinism: 48kHz run A vs run B ===" << std::endl;
    std::cout << "  checksumA=0x" << std::hex << run48kA.outputChecksum
               << " checksumB=0x" << run48kB.outputChecksum << std::dec
               << " match=" << (run48kA.outputChecksum == run48kB.outputChecksum ? "true" : "false") << std::endl;

    auto run96k = runSoak (sr96, blockSize, samples20MinAt96k, "96kHz run");
    printResult (run96k, "96kHz run (20min-equivalent)", sr96);

    std::cout << "\nSOAK TEST COMPLETE" << std::endl;
    return 0;
}
