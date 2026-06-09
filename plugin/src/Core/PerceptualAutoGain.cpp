#include "PerceptualAutoGain.h"

// ─────────────────────────────────────────────────────────────────────────────
void PerceptualAutoGain::prepareToPlay (double sampleRate, int samplesPerBlock, int numChannels)
{
    numChannels_ = numChannels;

    dryDetectorBuffer.setSize (numChannels, samplesPerBlock);
    wetDetectorBuffer.setSize (numChannels, samplesPerBlock);

    // 150 Hz High-Pass for perceptual energy detection (ignore sub-bass)
    juce::dsp::ProcessSpec hpfSpec { sampleRate,
                                      static_cast<juce::uint32> (samplesPerBlock),
                                      static_cast<juce::uint32> (numChannels) };

    auto hpfCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 150.0f);

    detectorHPF_dry.state = hpfCoeffs;
    detectorHPF_dry.prepare (hpfSpec);
    detectorHPF_dry.reset();

    detectorHPF_wet.state = hpfCoeffs;
    detectorHPF_wet.prepare (hpfSpec);
    detectorHPF_wet.reset();

    // EMA alpha for 400ms power window (block-rate)
    if (samplesPerBlock > 0 && sampleRate > 0.0)
    {
        double blocksPerSecond = sampleRate / static_cast<double> (samplesPerBlock);
        double timeConstantSec = 0.4;
        powerAlpha = static_cast<float> (1.0 - std::exp (-1.0 / (timeConstantSec * blocksPerSecond)));
    }

    smoothedInPower  = 0.0f;
    smoothedOutPower = 0.0f;
    currentGain      = 1.0f;
    targetGain       = 1.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// AUDIO THREAD — zero allocations below
// ─────────────────────────────────────────────────────────────────────────────
void PerceptualAutoGain::measureDryInput (const juce::AudioBuffer<float>& buffer, int numSamples)
{
    const int numCh = buffer.getNumChannels();

    // Copy dry input into detector buffer (channel-by-channel, zero-alloc)
    for (int ch = 0; ch < numCh; ++ch)
        dryDetectorBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    {
        juce::dsp::AudioBlock<float> dryBlock (dryDetectorBuffer);
        auto drySubBlock = dryBlock.getSubBlock (0, static_cast<size_t> (numSamples));
        juce::dsp::ProcessContextReplacing<float> dryCtx (drySubBlock);
        detectorHPF_dry.process (dryCtx);
    }

    // Mean-Square of HPF-filtered dry signal
    float inPower = 0.0f;
    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* data = dryDetectorBuffer.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
            inPower += data[i] * data[i];
    }
    inPower /= juce::jmax (static_cast<float> (numSamples * numCh), 1.0f);

    // Instant-catch from silence, else smooth
    if (smoothedInPower < 1e-7f)
        smoothedInPower = inPower;
    else
        smoothedInPower = smoothedInPower * (1.0f - powerAlpha) + inPower * powerAlpha;
}

// ─────────────────────────────────────────────────────────────────────────────
void PerceptualAutoGain::measureWetOutput (const juce::AudioBuffer<float>& buffer, int numSamples)
{
    const int numCh = buffer.getNumChannels();

    for (int ch = 0; ch < numCh; ++ch)
        wetDetectorBuffer.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    {
        juce::dsp::AudioBlock<float> wetBlock (wetDetectorBuffer);
        auto wetSubBlock = wetBlock.getSubBlock (0, static_cast<size_t> (numSamples));
        juce::dsp::ProcessContextReplacing<float> wetCtx (wetSubBlock);
        detectorHPF_wet.process (wetCtx);
    }

    float outPower = 0.0f;
    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* data = wetDetectorBuffer.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
            outPower += data[i] * data[i];
    }
    outPower /= juce::jmax (static_cast<float> (numSamples * numCh), 1.0f);

    if (smoothedOutPower < 1e-7f)
        smoothedOutPower = outPower;
    else
        smoothedOutPower = smoothedOutPower * (1.0f - powerAlpha) + outPower * powerAlpha;
}

// ─────────────────────────────────────────────────────────────────────────────
void PerceptualAutoGain::applyGain (juce::AudioBuffer<float>& buffer, int numSamples, bool enabled)
{
    if (enabled)
    {
        float inDb  = juce::Decibels::gainToDecibels (std::sqrt (smoothedInPower));
        float outDb = juce::Decibels::gainToDecibels (std::sqrt (smoothedOutPower));

        // Freeze gate: only update when both signals are audible
        if (inDb > -50.0f && outDb > -50.0f)
        {
            float diffDb = juce::jlimit (-18.0f, 18.0f, inDb - outDb);
            targetGain = juce::Decibels::decibelsToGain (diffDb);
        }
    }
    else
    {
        targetGain = 1.0f;
    }

    // Safety clamps
    if (std::isnan (targetGain) || std::isinf (targetGain)) targetGain = 1.0f;
    if (std::isnan (currentGain) || std::isinf (currentGain)) currentGain = 1.0f;
    targetGain = juce::jlimit (0.125f, 7.94f, targetGain);

    // Per-sample one-pole smoother
    const int numCh = buffer.getNumChannels();
    for (int i = 0; i < numSamples; ++i)
    {
        currentGain = currentGain * 0.995f + targetGain * 0.005f;
        currentGain = juce::jlimit (0.125f, 7.94f, currentGain);
        for (int ch = 0; ch < numCh; ++ch)
            buffer.getWritePointer (ch)[i] *= currentGain;
    }
}
