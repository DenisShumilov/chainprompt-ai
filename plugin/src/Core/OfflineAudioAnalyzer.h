#pragma once

#include <JuceHeader.h>

struct AudioMetrics;

/**
 * OfflineAudioAnalyzer — runs on MESSAGE THREAD only.
 *
 * Performs all offline analysis on the 4-second capture buffer:
 *   - Exact RMS, Peak, Crest Factor, ZCR
 *   - Mono/Stereo detection
 *   - 31-band 1/3-octave FFT spectrum
 *   - MD-TSER (Micro-Dynamic Transient-to-Sustain Energy Ratio)
 *   - Spectral Flux
 *   - OE-HER (Odd-to-Even Harmonic Energy Ratio)
 *   - BW-ICC (Band-Wise Inter-Channel Coherence)
 *   - L/R Energy Balance
 *
 * Stateless — all results written directly to AudioMetrics atomics.
 * Allocations are fine here (message thread).
 */
class OfflineAudioAnalyzer
{
public:
    OfflineAudioAnalyzer() = default;

    /** Run full offline analysis on captureBuffer. Writes results to metrics. */
    void analyze (const juce::AudioBuffer<float>& captureBuffer,
                  int totalSamples,
                  double sampleRate,
                  AudioMetrics& metrics);

private:
    /** Compute exact RMS, Peak, Crest, ZCR + mono/stereo detection. */
    void computeBasicMetrics (const juce::AudioBuffer<float>& buffer,
                              int totalSamples,
                              AudioMetrics& metrics);

    /** 31-band FFT spectrum + MD-TSER + Spectral Flux + OE-HER.
     *  Operates on mono-mixed data derived from captureBuffer. */
    void computeSpectralMetrics (const juce::AudioBuffer<float>& buffer,
                                 int totalSamples,
                                 double sampleRate,
                                 AudioMetrics& metrics);

    /** BW-ICC (4-band stereo coherence) + L/R balance. */
    void computeStereoMetrics (const juce::AudioBuffer<float>& buffer,
                               int totalSamples,
                               double sampleRate,
                               AudioMetrics& metrics);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OfflineAudioAnalyzer)
};
