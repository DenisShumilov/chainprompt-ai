#pragma once

#include <JuceHeader.h>

enum class CaptureState { Idle, Waiting, Recording, Done };

/**
 * AudioCaptureManager — Smart Wait audio capture + FLAC encoding.
 *
 * Audio thread methods (zero-alloc, lock-free):
 *   - processBlock(): Smart Wait state machine, copies samples to capture buffer
 *
 * Message thread methods:
 *   - encodeCaptureToBase64(): FLAC/WAV encode + Base64
 *   - prepareToPlay(): allocate capture buffer
 */
class AudioCaptureManager
{
public:
    AudioCaptureManager() = default;

    /** Allocate capture buffer (call from prepareToPlay). */
    void prepareToPlay (int numChannels, double sampleRate);

    /** Smart Wait: trigger on RMS > -45 dB, record 4 seconds of DRY input.
     *  Returns true when capture transitions to Done (triggers offline analysis).
     *  AUDIO THREAD — zero allocations. */
    bool processBlock (const juce::AudioBuffer<float>& inputBuffer,
                       int totalNumInputChannels);

    /** Encode captureBuffer to FLAC -> Base64. MESSAGE THREAD only. */
    void encodeCaptureToBase64 (double sampleRate);

    /** Save captureBuffer to a temp FLAC/WAV file. MESSAGE THREAD only.
     *  Returns the file path (empty on failure). Faster than Base64 for localhost.
     *  @param instanceId  Unique plugin instance ID to avoid file collisions
     *                     when multiple plugin instances capture simultaneously. */
    juce::String saveCaptureToTempFile (double sampleRate, const juce::String& instanceId = {});

    // ── State access ──
    CaptureState getState() const noexcept { return captureState.load (std::memory_order_relaxed); }
    void setState (CaptureState s) noexcept { captureState.store (s, std::memory_order_relaxed); }

    int getWritePosition() const noexcept { return captureWritePosition.load (std::memory_order_relaxed); }
    int getTotalSamples() const noexcept { return captureTotalSamples.load (std::memory_order_relaxed); }

    const juce::AudioBuffer<float>& getCaptureBuffer() const noexcept { return captureBuffer; }

    const juce::String& getAudioBase64() const noexcept { return capturedAudioBase64; }
    const juce::String& getAudioMimeType() const noexcept { return capturedAudioMimeType; }

    // ── Public atomics for cross-thread state (backward compat with Editor reads) ──
    std::atomic<CaptureState> captureState { CaptureState::Idle };
    std::atomic<int> captureWritePosition { 0 };
    std::atomic<int> captureTotalSamples { 0 };

    // Frozen metrics: snapshot taken at capture completion time.
    // Send these to the server instead of live metrics to avoid desync.
    struct FrozenMetrics
    {
        float rmsDB = -100.0f;
        float peakDB = -100.0f;
        float crestFactor = 0.0f;
        float zcr = 0.0f;
        double bpm = 120.0;
        float subPct = 0.0f;
        float lowPct = 0.0f;
        float lowMidPct = 0.0f;
        float highMidPct = 0.0f;
        float airPct = 0.0f;
        bool isStereo = true;
        std::array<float, 31> spectrum31 {};
        bool spectrum31Ready = false;
        float mdTserDb = 0.0f;
        float spectralFlux = 0.0f;
        float oeHerDb = 0.0f;
        bool isTonal = false;
        std::array<float, 4> bwIcc {{ 1.0f, 1.0f, 1.0f, 1.0f }};
        float lrBalanceDb = 0.0f;
    };
    FrozenMetrics frozenMetrics;

    // Message thread only
    juce::AudioBuffer<float> captureBuffer;
    juce::String capturedAudioBase64;
    juce::String capturedAudioMimeType;
    juce::String capturedAudioPath;  // temp file path (preferred over Base64)

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioCaptureManager)
};
