#include "AudioCaptureManager.h"

// ─────────────────────────────────────────────────────────────────────────────
void AudioCaptureManager::prepareToPlay (int numChannels, double sampleRate)
{
    captureTotalSamples.store (static_cast<int> (sampleRate * 4.0));
    captureBuffer.setSize (numChannels, captureTotalSamples.load());
    captureBuffer.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// AUDIO THREAD — zero allocations
// ─────────────────────────────────────────────────────────────────────────────
bool AudioCaptureManager::processBlock (const juce::AudioBuffer<float>& inputBuffer,
                                         int totalNumInputChannels)
{
    auto state = captureState.load (std::memory_order_relaxed);

    if (state == CaptureState::Waiting)
    {
        // Detect audio presence via RMS > -45 dB
        const int chCount = juce::jmin (totalNumInputChannels, inputBuffer.getNumChannels());
        const int nSamp   = inputBuffer.getNumSamples();
        float sumSq = 0.0f;
        for (int ch = 0; ch < chCount; ++ch)
        {
            const float* data = inputBuffer.getReadPointer (ch);
            for (int s = 0; s < nSamp; ++s)
                sumSq += data[s] * data[s];
        }
        const float rmsLin = std::sqrt (sumSq / juce::jmax (static_cast<float> (nSamp * chCount), 1.0f));
        const float rmsdB  = (rmsLin > 0.0f)
                             ? 20.0f * std::log10 (rmsLin)
                             : -100.0f;

        if (rmsdB > -45.0f)
        {
            captureWritePosition.store (0, std::memory_order_relaxed);
            captureState.store (CaptureState::Recording, std::memory_order_relaxed);
            state = CaptureState::Recording;
        }
    }

    if (state == CaptureState::Recording)
    {
        const int writePos = captureWritePosition.load (std::memory_order_relaxed);
        const int total    = captureTotalSamples.load (std::memory_order_relaxed);
        const int samplesToCopy = juce::jmin (inputBuffer.getNumSamples(), total - writePos);

        if (samplesToCopy > 0)
        {
            const int capChannels = juce::jmin (totalNumInputChannels, captureBuffer.getNumChannels());
            for (int ch = 0; ch < capChannels; ++ch)
                captureBuffer.copyFrom (ch, writePos, inputBuffer, ch, 0, samplesToCopy);

            const int newPos = writePos + samplesToCopy;
            captureWritePosition.store (newPos, std::memory_order_relaxed);

            if (newPos >= total)
            {
                // Release: guarantees all captureBuffer writes are visible
                // to the message thread that will acquire-load this state.
                captureState.store (CaptureState::Done, std::memory_order_release);
                return true;  // signal: capture complete, trigger async update
            }
        }
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// MESSAGE THREAD — allocations OK
// ─────────────────────────────────────────────────────────────────────────────
void AudioCaptureManager::encodeCaptureToBase64 (double sampleRate)
{
    const int totalSamples = captureTotalSamples.load();
    const int numChannels  = captureBuffer.getNumChannels();
    const double sr = sampleRate > 0 ? sampleRate : 44100.0;

    juce::MemoryBlock encodedBlock;
    juce::String mimeType;

    juce::FlacAudioFormat flacFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        flacFormat.createWriterFor (new juce::MemoryOutputStream (encodedBlock, false),
                                    sr,
                                    static_cast<unsigned int> (numChannels),
                                    16, {}, 0));

    if (writer != nullptr)
    {
        mimeType = "audio/flac";
    }
    else
    {
        juce::WavAudioFormat wavFormat;
        writer.reset (wavFormat.createWriterFor (new juce::MemoryOutputStream (encodedBlock, false),
                                                  sr,
                                                  static_cast<unsigned int> (numChannels),
                                                  16, {}, 0));
        mimeType = "audio/wav";
    }

    if (writer != nullptr)
    {
        writer->writeFromAudioSampleBuffer (captureBuffer, 0, totalSamples);
        writer.reset();

        capturedAudioBase64 = juce::Base64::toBase64 (encodedBlock.getData(),
                                                       encodedBlock.getSize());
        capturedAudioMimeType = mimeType;

        DBG ("encodeCaptureToBase64: " + mimeType + ", "
             + juce::String (static_cast<int> (encodedBlock.getSize())) + " bytes -> "
             + juce::String (capturedAudioBase64.length()) + " Base64 chars");
    }
    else
    {
        capturedAudioBase64 = {};
        capturedAudioMimeType = {};
        DBG ("encodeCaptureToBase64: FAILED — no encoder available");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Save capture to temp file (faster than Base64 for localhost server)
// ─────────────────────────────────────────────────────────────────────────────
juce::String AudioCaptureManager::saveCaptureToTempFile (double sampleRate, const juce::String& instanceId)
{
    const int totalSamples = captureTotalSamples.load();
    const int numChannels  = captureBuffer.getNumChannels();
    const double sr = sampleRate > 0 ? sampleRate : 44100.0;

    // Use ChainPrompt temp directory with unique filename per instance
    auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("ChainPromptAI");
    tempDir.createDirectory();

    juce::String suffix = instanceId.isNotEmpty() ? ("_" + instanceId.substring (0, 8)) : "";
    juce::File tempFile = tempDir.getChildFile ("capture" + suffix + ".flac");
    juce::String mimeType;

    // Try FLAC first, fallback to WAV
    {
        juce::FlacAudioFormat flacFormat;
        auto outStream = tempFile.createOutputStream();
        if (outStream != nullptr)
        {
            // createWriterFor takes ownership of the raw pointer on success
            auto* rawStream = outStream.release();
            std::unique_ptr<juce::AudioFormatWriter> writer (
                flacFormat.createWriterFor (rawStream, sr,
                                            static_cast<unsigned int> (numChannels),
                                            16, {}, 0));
            if (writer != nullptr)
            {
                writer->writeFromAudioSampleBuffer (captureBuffer, 0, totalSamples);
                writer.reset();
                mimeType = "audio/flac";
            }
            else
            {
                delete rawStream;  // FLAC failed, try WAV
                tempFile = tempDir.getChildFile ("capture" + suffix + ".wav");
                auto wavStream = tempFile.createOutputStream();
                if (wavStream != nullptr)
                {
                    auto* rawWavStream = wavStream.release();
                    juce::WavAudioFormat wavFormat;
                    std::unique_ptr<juce::AudioFormatWriter> wavWriter (
                        wavFormat.createWriterFor (rawWavStream, sr,
                                                    static_cast<unsigned int> (numChannels),
                                                    16, {}, 0));
                    if (wavWriter != nullptr)
                    {
                        wavWriter->writeFromAudioSampleBuffer (captureBuffer, 0, totalSamples);
                        wavWriter.reset();
                        mimeType = "audio/wav";
                    }
                    else
                    {
                        delete rawWavStream;
                    }
                }
            }
        }
    }

    if (mimeType.isNotEmpty())
    {
        capturedAudioPath = tempFile.getFullPathName();
        capturedAudioMimeType = mimeType;
        capturedAudioBase64 = {};  // Clear Base64 — we use file path now

        DBG ("saveCaptureToTempFile: " + mimeType + " -> " + capturedAudioPath
             + " (" + juce::String (static_cast<int> (tempFile.getSize())) + " bytes)");
    }
    else
    {
        capturedAudioPath = {};
        DBG ("saveCaptureToTempFile: FAILED — no encoder available");
    }

    return capturedAudioPath;
}
