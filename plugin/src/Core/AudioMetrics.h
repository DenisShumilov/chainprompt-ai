#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

// ── Audio analysis metrics (written on audio thread, read on message thread) ──
// Extracted from ChainPromptAudioProcessor.h for decoupled access by subsystems.

struct AudioMetrics
{
    std::atomic<float> rmsDB   { -100.0f };
    std::atomic<float> peakDB  { -100.0f };
    std::atomic<float> crestFactor { 0.0f };
    std::atomic<float> zcr     { 0.0f };
    std::atomic<double> bpm    { 120.0 };

    // Spectral balance (5-band energy as % of total)
    std::atomic<float> subPct     { 0.0f };
    std::atomic<float> lowPct     { 0.0f };
    std::atomic<float> lowMidPct  { 0.0f };
    std::atomic<float> highMidPct { 0.0f };
    std::atomic<float> airPct     { 0.0f };

    // 31-band 1/3-octave spectrum (computed offline from 4-sec capture)
    static constexpr int kNumSpectrumBands = 31;
    std::array<std::atomic<float>, 31> spectrum31 {};
    std::atomic<bool> spectrum31Ready { false };

    // MD-TSER: Micro-Dynamic Transient-to-Sustain Energy Ratio (dB)
    std::atomic<float> mdTserDb { 0.0f };

    // OE-HER: Odd-to-Even Harmonic Energy Ratio (dB)
    std::atomic<float> oeHerDb { 0.0f };
    std::atomic<bool>  isTonal { false };

    // Spectral Flux (normalised 0-1)
    std::atomic<float> spectralFlux { 0.0f };

    // BW-ICC: Band-Wise Inter-Channel Coherence
    static constexpr int kNumIccBands = 4;
    std::array<std::atomic<float>, 4> bwIcc { { {1.0f}, {1.0f}, {1.0f}, {1.0f} } };

    // L/R Energy Balance (dB)
    std::atomic<float> lrBalanceDb { 0.0f };

    // Diagnostic
    std::atomic<float> diagPreRms  { 0.0f };
    std::atomic<float> diagPostRms { 0.0f };
    std::atomic<bool>  diagGraphBypassed { false };
    std::atomic<float> diagAutoGain { 1.0f };
    std::atomic<int>   diagFifoApplied { 0 };
    std::atomic<int>   diagParamsSet { 0 };

    // Track channel configuration
    std::atomic<bool>  isStereo { true };
};
