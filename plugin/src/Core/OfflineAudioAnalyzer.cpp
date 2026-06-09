#include "OfflineAudioAnalyzer.h"
#include "AudioMetrics.h"

// ─────────────────────────────────────────────────────────────────────────────
// Public entry point
// ─────────────────────────────────────────────────────────────────────────────
void OfflineAudioAnalyzer::analyze (const juce::AudioBuffer<float>& captureBuffer,
                                    int totalSamples,
                                    double sampleRate,
                                    AudioMetrics& metrics)
{
    computeBasicMetrics (captureBuffer, totalSamples, metrics);
    computeSpectralMetrics (captureBuffer, totalSamples, sampleRate, metrics);
    computeStereoMetrics (captureBuffer, totalSamples, sampleRate, metrics);
}

// ─────────────────────────────────────────────────────────────────────────────
// Basic metrics: RMS, Peak, Crest, ZCR, Mono/Stereo detection
// ─────────────────────────────────────────────────────────────────────────────
void OfflineAudioAnalyzer::computeBasicMetrics (const juce::AudioBuffer<float>& buffer,
                                                 int totalSamples,
                                                 AudioMetrics& metrics)
{
    const int numChannels = buffer.getNumChannels();

    float sumSquares = 0.0f;
    float peak       = 0.0f;
    int   zeroCrossings = 0;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* data = buffer.getReadPointer (ch);
        float prev = data[0];

        for (int i = 0; i < totalSamples; ++i)
        {
            const float s = data[i];
            sumSquares += s * s;

            const float absS = std::abs (s);
            if (absS > peak) peak = absS;

            if ((s > 0.0f && prev < 0.0f) || (s < 0.0f && prev > 0.0f))
                ++zeroCrossings;

            prev = s;
        }
    }

    const float totalSamplesF = static_cast<float> (totalSamples * juce::jmax (numChannels, 1));
    const float rmsLinear = std::sqrt (sumSquares / juce::jmax (totalSamplesF, 1.0f));

    float rmsDB  = juce::Decibels::gainToDecibels (rmsLinear, -100.0f);
    float peakDB = juce::Decibels::gainToDecibels (peak, -100.0f);
    float crest  = peakDB - rmsDB;
    float zcrNorm = static_cast<float> (zeroCrossings) / juce::jmax (totalSamplesF, 1.0f);

    rmsDB   = juce::jmax (rmsDB, -100.0f);
    peakDB  = juce::jmax (peakDB, -100.0f);
    crest   = juce::jlimit (0.0f, 60.0f, crest);
    zcrNorm = juce::jlimit (0.0f, 1.0f, zcrNorm);

    metrics.rmsDB.store (rmsDB, std::memory_order_relaxed);
    metrics.peakDB.store (peakDB, std::memory_order_relaxed);
    metrics.crestFactor.store (crest, std::memory_order_relaxed);
    metrics.zcr.store (zcrNorm, std::memory_order_relaxed);

    // Mono/Stereo detection
    bool isTrueStereo = false;
    if (numChannels > 1)
    {
        const float* left  = buffer.getReadPointer (0);
        const float* right = buffer.getReadPointer (1);
        for (int i = 0; i < totalSamples; ++i)
        {
            if (std::abs (left[i] - right[i]) > 0.0001f)
            {
                isTrueStereo = true;
                break;
            }
        }
    }
    // NOTE: Do NOT overwrite metrics.isStereo here!
    // isStereo is set once in prepareToPlay() from the DAW bus configuration.
    // Content-based L==R detection (isTrueStereo) must NOT change the track format,
    // because mono-summing plugins (e.g., guitar cabinets) make L==R while the
    // DAW track remains stereo. Overwriting here causes "stereo amnesia" where
    // the server starts picking Mono plugin UIDs and destroys the stereo image.
}

// ─────────────────────────────────────────────────────────────────────────────
// Spectral metrics: 31-band FFT, MD-TSER, Spectral Flux, OE-HER
// ─────────────────────────────────────────────────────────────────────────────
void OfflineAudioAnalyzer::computeSpectralMetrics (const juce::AudioBuffer<float>& buffer,
                                                    int totalSamples,
                                                    double sampleRate,
                                                    AudioMetrics& metrics)
{
    const int numChannels = buffer.getNumChannels();
    const double sr = sampleRate > 0 ? sampleRate : 44100.0;

    // ISO 1/3-octave center frequencies (Hz)
    static constexpr float kCenterFreqs[31] = {
        20.0f, 25.0f, 31.5f, 40.0f, 50.0f, 63.0f, 80.0f, 100.0f,
        125.0f, 160.0f, 200.0f, 250.0f, 315.0f, 400.0f, 500.0f,
        630.0f, 800.0f, 1000.0f, 1250.0f, 1600.0f, 2000.0f, 2500.0f,
        3150.0f, 4000.0f, 5000.0f, 6300.0f, 8000.0f, 10000.0f,
        12500.0f, 16000.0f, 20000.0f
    };
    static constexpr float kOctaveFactor = 1.122462f;  // 2^(1/6)

    constexpr int fftOrder = 12;
    constexpr int fftSize  = 1 << fftOrder;  // 4096
    juce::dsp::FFT fft (fftOrder);

    std::array<double, 31> bandAccum {};
    bandAccum.fill (0.0);
    int numFrames = 0;

    // Hann window
    std::array<float, fftSize> window;
    for (int i = 0; i < fftSize; ++i)
        window[static_cast<size_t> (i)] = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::twoPi
                                                                      * static_cast<float> (i)
                                                                      / static_cast<float> (fftSize)));

    const int hopSize = fftSize / 2;

    // Mono-mix: average channels
    std::vector<float> monoData (static_cast<size_t> (totalSamples), 0.0f);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* chData = buffer.getReadPointer (ch);
        for (int i = 0; i < totalSamples; ++i)
            monoData[static_cast<size_t> (i)] += chData[i];
    }
    if (numChannels > 1)
    {
        const float invCh = 1.0f / static_cast<float> (numChannels);
        for (int i = 0; i < totalSamples; ++i)
            monoData[static_cast<size_t> (i)] *= invCh;
    }

    std::vector<float> fftData (static_cast<size_t> (fftSize * 2), 0.0f);

    const int numBins = fftSize / 2;
    std::vector<double> avgMagSpectrum (static_cast<size_t> (numBins), 0.0);

    // Spectral Flux accumulators
    std::vector<float> prevMag (static_cast<size_t> (numBins), 0.0f);
    double fluxAccum = 0.0;
    double magAccum  = 0.0;
    int    fluxFrames = 0;

    const float binWidth = static_cast<float> (sr) / static_cast<float> (fftSize);

    for (int frameStart = 0; frameStart + fftSize <= totalSamples; frameStart += hopSize)
    {
        for (int i = 0; i < fftSize; ++i)
            fftData[static_cast<size_t> (i)] = monoData[static_cast<size_t> (frameStart + i)]
                                               * window[static_cast<size_t> (i)];
        for (int i = fftSize; i < fftSize * 2; ++i)
            fftData[static_cast<size_t> (i)] = 0.0f;

        fft.performFrequencyOnlyForwardTransform (fftData.data());

        // Spectral Flux
        if (numFrames > 0)
        {
            double diffSumSq = 0.0;
            double frameMagSum = 0.0;
            for (int bin = 1; bin < numBins; ++bin)
            {
                const float curMag = fftData[static_cast<size_t> (bin)];
                const float delta  = curMag - prevMag[static_cast<size_t> (bin)];
                diffSumSq  += static_cast<double> (delta) * static_cast<double> (delta);
                frameMagSum += static_cast<double> (curMag);
            }
            fluxAccum += std::sqrt (diffSumSq);
            magAccum  += frameMagSum;
            ++fluxFrames;
        }

        for (int bin = 0; bin < numBins; ++bin)
            prevMag[static_cast<size_t> (bin)] = fftData[static_cast<size_t> (bin)];

        for (int bin = 0; bin < numBins; ++bin)
            avgMagSpectrum[static_cast<size_t> (bin)] += static_cast<double> (fftData[static_cast<size_t> (bin)]);

        // Accumulate into 31 bands
        for (int band = 0; band < 31; ++band)
        {
            const float center = kCenterFreqs[band];
            const float lo = center / kOctaveFactor;
            const float hi = center * kOctaveFactor;

            int binLo = juce::jmax (1, static_cast<int> (std::ceil (lo / binWidth)));
            int binHi = juce::jmin (fftSize / 2 - 1, static_cast<int> (std::floor (hi / binWidth)));

            double sumSq = 0.0;
            int count = 0;
            for (int bin = binLo; bin <= binHi; ++bin)
            {
                float mag = fftData[static_cast<size_t> (bin)];
                sumSq += static_cast<double> (mag * mag);
                ++count;
            }
            if (count > 0)
                bandAccum[static_cast<size_t> (band)] += std::sqrt (sumSq / static_cast<double> (count));
        }
        ++numFrames;
    }

    // Finalize averaged magnitude spectrum
    if (numFrames > 0)
    {
        for (int bin = 0; bin < numBins; ++bin)
            avgMagSpectrum[static_cast<size_t> (bin)] /= static_cast<double> (numFrames);
    }

    // Average across frames and convert to dB
    if (numFrames > 0)
    {
        for (int band = 0; band < 31; ++band)
        {
            float avgMag = static_cast<float> (bandAccum[static_cast<size_t> (band)]
                                                / static_cast<double> (numFrames));
            float dB = juce::Decibels::gainToDecibels (avgMag, -100.0f);
            dB = juce::jmax (dB, -100.0f);
            metrics.spectrum31[static_cast<size_t> (band)].store (dB, std::memory_order_relaxed);
        }
        metrics.spectrum31Ready.store (true, std::memory_order_relaxed);
    }

    juce::Logger::writeToLog ("31-band spectrum computed: " + juce::String (numFrames) + " FFT frames");

    // ── MD-TSER: Micro-Dynamic Transient-to-Sustain Energy Ratio ──
    {
        const float srf = static_cast<float> (sr);

        auto timeToAlpha = [srf](float ms) -> float {
            return 1.0f - std::exp (-1.0f / (ms * 0.001f * srf));
        };

        const float fastAttack  = timeToAlpha (1.0f);
        const float fastRelease = timeToAlpha (20.0f);
        const float slowAttack  = timeToAlpha (15.0f);
        const float slowRelease = timeToAlpha (100.0f);

        float fastEnv = 0.0f;
        float slowEnv = 0.0f;
        double transientSumSq = 0.0;
        double sustainSumSq   = 0.0;

        for (int i = 0; i < totalSamples; ++i)
        {
            const float absVal = std::abs (monoData[static_cast<size_t> (i)]);

            const float fa = (absVal > fastEnv) ? fastAttack : fastRelease;
            fastEnv += fa * (absVal - fastEnv);

            const float sa = (absVal > slowEnv) ? slowAttack : slowRelease;
            slowEnv += sa * (absVal - slowEnv);

            const float transient = juce::jmax (0.0f, fastEnv - slowEnv);

            transientSumSq += static_cast<double> (transient) * static_cast<double> (transient);
            sustainSumSq   += static_cast<double> (slowEnv)   * static_cast<double> (slowEnv);
        }

        const double n = static_cast<double> (totalSamples);
        const float transientRms = static_cast<float> (std::sqrt (transientSumSq / n));
        const float sustainRms  = static_cast<float> (std::sqrt (sustainSumSq / n));

        float mdTser = 20.0f * std::log10 (transientRms / (sustainRms + 1e-6f));
        mdTser = juce::jlimit (-20.0f, 20.0f, mdTser);
        if (std::isnan (mdTser) || std::isinf (mdTser))
            mdTser = 0.0f;

        metrics.mdTserDb.store (mdTser, std::memory_order_relaxed);

        juce::Logger::writeToLog ("MD-TSER computed: " + juce::String (mdTser, 2) + " dB"
            + " (transientRMS=" + juce::String (transientRms, 6)
            + " sustainRMS=" + juce::String (sustainRms, 6) + ")");
    }

    // ── Spectral Flux: finalise normalised 0-1 value ──
    {
        float flux = 0.0f;
        if (fluxFrames > 0 && magAccum > 1e-12)
        {
            const double avgFlux = fluxAccum / static_cast<double> (fluxFrames);
            const double avgMag  = magAccum  / static_cast<double> (fluxFrames);
            flux = static_cast<float> (juce::jlimit (0.0, 1.0, (avgFlux / avgMag) * 5.0));
        }
        if (std::isnan (flux) || std::isinf (flux))
            flux = 0.0f;

        metrics.spectralFlux.store (flux, std::memory_order_relaxed);
        juce::Logger::writeToLog ("Spectral Flux computed: " + juce::String (flux, 3));
    }

    // ── OE-HER: Odd-to-Even Harmonic Energy Ratio ──
    {
        float oeHer = 0.0f;
        bool  tonal = false;

        const int binLo_f0 = juce::jmax (1, static_cast<int> (std::ceil  (60.0f  / binWidth)));
        const int binHi_f0 = juce::jmin (numBins - 1, static_cast<int> (std::floor (2000.0f / binWidth)));

        int    f0Bin = binLo_f0;
        double f0Mag = 0.0;
        for (int bin = binLo_f0; bin <= binHi_f0; ++bin)
        {
            if (avgMagSpectrum[static_cast<size_t> (bin)] > f0Mag)
            {
                f0Mag = avgMagSpectrum[static_cast<size_t> (bin)];
                f0Bin = bin;
            }
        }

        // Tonality check
        {
            std::vector<double> sortedMags;
            sortedMags.reserve (static_cast<size_t> (binHi_f0 - binLo_f0 + 1));
            for (int bin = binLo_f0; bin <= binHi_f0; ++bin)
                sortedMags.push_back (avgMagSpectrum[static_cast<size_t> (bin)]);

            std::sort (sortedMags.begin(), sortedMags.end());
            const double median = sortedMags[sortedMags.size() / 2];

            const double medianDb = (median > 0.0) ? 20.0 * std::log10 (median) : -100.0;
            const double f0Db     = (f0Mag > 0.0)  ? 20.0 * std::log10 (f0Mag)  : -100.0;
            tonal = (f0Db - medianDb) > 12.0;
        }

        if (tonal && f0Bin > 0)
        {
            const float f0Hz = static_cast<float> (f0Bin) * binWidth;

            double evenEnergy = 0.0;
            double oddEnergy  = 0.0;

            auto getHarmonicMag = [&](int harmonic) -> double {
                int bin = static_cast<int> (std::round (f0Hz * static_cast<float> (harmonic) / binWidth));
                if (bin >= 1 && bin < numBins)
                {
                    double m = avgMagSpectrum[static_cast<size_t> (bin)];
                    if (bin > 1)
                        m = juce::jmax (m, avgMagSpectrum[static_cast<size_t> (bin - 1)]);
                    if (bin < numBins - 1)
                        m = juce::jmax (m, avgMagSpectrum[static_cast<size_t> (bin + 1)]);
                    return m;
                }
                return 0.0;
            };

            for (int h = 2; h <= 8; h += 2)
            {
                double m = getHarmonicMag (h);
                evenEnergy += m * m;
            }
            for (int h = 3; h <= 9; h += 2)
            {
                double m = getHarmonicMag (h);
                oddEnergy += m * m;
            }

            oeHer = 10.0f * std::log10 (static_cast<float> ((oddEnergy + 1e-12) / (evenEnergy + 1e-12)));
            oeHer = juce::jlimit (-20.0f, 20.0f, oeHer);
            if (std::isnan (oeHer) || std::isinf (oeHer))
                oeHer = 0.0f;

            juce::Logger::writeToLog ("OE-HER computed: " + juce::String (oeHer, 2) + " dB"
                + " (f0=" + juce::String (f0Hz, 1) + " Hz, tonal=YES"
                + ", oddE=" + juce::String (oddEnergy, 4)
                + ", evenE=" + juce::String (evenEnergy, 4) + ")");
        }
        else
        {
            juce::Logger::writeToLog ("OE-HER: atonal/noise — no strong fundamental detected");
        }

        metrics.oeHerDb.store (oeHer, std::memory_order_relaxed);
        metrics.isTonal.store (tonal, std::memory_order_relaxed);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stereo metrics: BW-ICC + L/R Balance
// ─────────────────────────────────────────────────────────────────────────────
void OfflineAudioAnalyzer::computeStereoMetrics (const juce::AudioBuffer<float>& buffer,
                                                  int totalSamples,
                                                  double sampleRate,
                                                  AudioMetrics& metrics)
{
    const int numChannels = buffer.getNumChannels();

    if (numChannels < 2 || totalSamples <= 0)
    {
        // Mono track: perfect correlation, centered balance
        for (int i = 0; i < 4; ++i)
            metrics.bwIcc[static_cast<size_t> (i)].store (1.0f, std::memory_order_relaxed);
        metrics.lrBalanceDb.store (0.0f, std::memory_order_relaxed);
        return;
    }

    const size_t n = static_cast<size_t> (totalSamples);

    std::vector<float> leftRaw  (n);
    std::vector<float> rightRaw (n);
    {
        const float* lPtr = buffer.getReadPointer (0);
        const float* rPtr = buffer.getReadPointer (1);
        std::copy (lPtr, lPtr + totalSamples, leftRaw.begin());
        std::copy (rPtr, rPtr + totalSamples, rightRaw.begin());
    }

    // Band definitions: [lowCut, highCut] in Hz
    struct BandDef { float lo; float hi; };
    static constexpr std::array<BandDef, 4> kBands = {{
        {  0.0f,  100.0f },
        { 100.0f, 500.0f },
        { 500.0f, 4000.0f },
        { 4000.0f, 0.0f }
    }};

    auto pearson = [](const std::vector<float>& a, const std::vector<float>& b, size_t count) -> float {
        double sumLR = 0.0, sumLL = 0.0, sumRR = 0.0;
        for (size_t i = 0; i < count; ++i)
        {
            const double l = static_cast<double> (a[i]);
            const double r = static_cast<double> (b[i]);
            sumLR += l * r;
            sumLL += l * l;
            sumRR += r * r;
        }
        const double denom = std::sqrt (sumLL * sumRR);
        if (denom < 1e-12)
            return 1.0f;
        return static_cast<float> (sumLR / denom);
    };

    auto applyIIR = [](std::vector<float>& data, const juce::dsp::IIR::Coefficients<float>::Ptr& coeffs) {
        juce::dsp::IIR::Filter<float> filter;
        filter.coefficients = coeffs;
        filter.reset();
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = filter.processSample (data[i]);
    };

    for (int band = 0; band < 4; ++band)
    {
        const auto& def = kBands[static_cast<size_t> (band)];

        std::vector<float> lFilt (leftRaw);
        std::vector<float> rFilt (rightRaw);

        // Cascaded 4th-order Butterworth (24 dB/oct)
        if (def.lo > 0.0f)
        {
            auto hpfCoeffs = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, static_cast<double> (def.lo));
            applyIIR (lFilt, hpfCoeffs);
            applyIIR (rFilt, hpfCoeffs);
            applyIIR (lFilt, hpfCoeffs);
            applyIIR (rFilt, hpfCoeffs);
        }

        if (def.hi > 0.0f)
        {
            auto lpfCoeffs = juce::dsp::IIR::Coefficients<float>::makeLowPass (sampleRate, static_cast<double> (def.hi));
            applyIIR (lFilt, lpfCoeffs);
            applyIIR (rFilt, lpfCoeffs);
            applyIIR (lFilt, lpfCoeffs);
            applyIIR (rFilt, lpfCoeffs);
        }

        float icc = pearson (lFilt, rFilt, n);
        icc = juce::jlimit (-1.0f, 1.0f, icc);
        if (std::isnan (icc))
            icc = 1.0f;

        metrics.bwIcc[static_cast<size_t> (band)].store (icc, std::memory_order_relaxed);
    }

    juce::Logger::writeToLog ("BW-ICC computed: Sub="
        + juce::String (metrics.bwIcc[0].load (std::memory_order_relaxed), 3)
        + " LowMid=" + juce::String (metrics.bwIcc[1].load (std::memory_order_relaxed), 3)
        + " HiMid=" + juce::String (metrics.bwIcc[2].load (std::memory_order_relaxed), 3)
        + " High=" + juce::String (metrics.bwIcc[3].load (std::memory_order_relaxed), 3));

    // ── L/R Energy Balance ──
    {
        double sumL = 0.0, sumR = 0.0;
        const float* lPtr = buffer.getReadPointer (0);
        const float* rPtr = buffer.getReadPointer (1);
        for (int i = 0; i < totalSamples; ++i)
        {
            sumL += static_cast<double> (lPtr[i]) * static_cast<double> (lPtr[i]);
            sumR += static_cast<double> (rPtr[i]) * static_cast<double> (rPtr[i]);
        }
        const double rmsL = std::sqrt (sumL / std::max (totalSamples, 1));
        const double rmsR = std::sqrt (sumR / std::max (totalSamples, 1));

        float balanceDb = 0.0f;
        if (rmsL > 1e-10 && rmsR > 1e-10)
            balanceDb = static_cast<float> (20.0 * std::log10 (rmsL / rmsR));
        else if (rmsL > 1e-10)
            balanceDb = 60.0f;
        else if (rmsR > 1e-10)
            balanceDb = -60.0f;

        balanceDb = juce::jlimit (-60.0f, 60.0f, balanceDb);
        if (std::isnan (balanceDb) || std::isinf (balanceDb))
            balanceDb = 0.0f;

        metrics.lrBalanceDb.store (balanceDb, std::memory_order_relaxed);
        juce::Logger::writeToLog ("L/R Balance: " + juce::String (balanceDb, 2) + " dB");
    }
}
