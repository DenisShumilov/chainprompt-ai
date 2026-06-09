#pragma once

#include <JuceHeader.h>

/**
 * PerceptualAutoGain — HPF-based loudness matching.
 *
 * All methods called from AUDIO THREAD (processBlock) — zero allocations.
 * Buffers pre-allocated in prepareToPlay().
 *
 * Usage in processBlock:
 *   1. measureDryInput(buffer)    — before graph processing
 *   2. [graph processes buffer]
 *   3. measureWetOutput(buffer)   — after graph processing
 *   4. applyGain(buffer, enabled) — compensate loudness difference
 */
class PerceptualAutoGain
{
public:
    PerceptualAutoGain() = default;

    /** Allocate detector buffers & HPF filters. Call from prepareToPlay(). */
    void prepareToPlay (double sampleRate, int samplesPerBlock, int numChannels);

    /** Step 1: Measure DRY input power through 150 Hz HPF. AUDIO THREAD. */
    void measureDryInput (const juce::AudioBuffer<float>& buffer, int numSamples);

    /** Step 3: Measure WET output power through 150 Hz HPF. AUDIO THREAD. */
    void measureWetOutput (const juce::AudioBuffer<float>& buffer, int numSamples);

    /** Step 4+5: Calculate gain + apply per-sample smoothing. AUDIO THREAD. */
    void applyGain (juce::AudioBuffer<float>& buffer, int numSamples, bool enabled);

    /** Current target gain for diagnostics. */
    float getTargetGain() const noexcept { return targetGain; }

private:
    // HPF filters (150 Hz, one per sidechain)
    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
                                    juce::dsp::IIR::Coefficients<float>> detectorHPF_dry;
    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
                                    juce::dsp::IIR::Coefficients<float>> detectorHPF_wet;

    // Pre-allocated detector buffers
    juce::AudioBuffer<float> dryDetectorBuffer;
    juce::AudioBuffer<float> wetDetectorBuffer;

    // Power-domain EMA state (audio thread only)
    float smoothedInPower  = 0.0f;
    float smoothedOutPower = 0.0f;
    float powerAlpha       = 0.0f;   // EMA coefficient for 400ms window
    float targetGain       = 1.0f;
    float currentGain      = 1.0f;
    int   numChannels_     = 2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PerceptualAutoGain)
};
