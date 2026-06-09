#include "ChainPromptAudioProcessor.h"
#include "ChainStateSerializer.h"
#include "../GUI/ChainPromptAudioProcessorEditor.h"

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <unistd.h>
#endif

// ── File logger: writes all DBG/Logger output to a file we can inspect ──
static std::unique_ptr<juce::FileLogger> sFileLogger;

static void ensureFileLogger()
{
    if (sFileLogger == nullptr)
    {
        auto logFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                           .getChildFile("ChainPrompt AI")
                           .getChildFile("debug.log");
        sFileLogger = std::make_unique<juce::FileLogger>(logFile, "ChainPrompt AI Debug Log", 0);
        juce::Logger::setCurrentLogger(sFileLogger.get());
    }
}

ChainPromptAudioProcessor::ChainPromptAudioProcessor()
#ifndef JucePlugin_PreferredChannelConfigurations
    : AudioProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::stereo(), true)
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      graphRebuilder (*this)
#endif
{
  ensureFileLogger();

  instanceId = juce::Uuid().toString();

  // Session scope: PID-based default. All instances in the same DAW process
  // share one scope token. Future Master Mode can override with project hash.
#if JUCE_WINDOWS
  projectScope = "pid-" + juce::String (static_cast<int> (GetCurrentProcessId()));
#else
  projectScope = "pid-" + juce::String (static_cast<int> (getpid()));
#endif

  mainGraph = std::make_unique<juce::AudioProcessorGraph>();

  // CRITICAL: Configure graph bus layout IMMEDIATELY after creation.
  // ── Stereo-only graph policy: no sidechain bus ──
  {
    juce::AudioProcessor::BusesLayout stereoLayout;
    stereoLayout.inputBuses.add (juce::AudioChannelSet::stereo());
    stereoLayout.outputBuses.add (juce::AudioChannelSet::stereo());
    mainGraph->setBusesLayout (stereoLayout);
  }

  // Register for global scan completion (with removable listener ID).
  scanListenerId = pluginCache->addListener ([this]() {
    if (hasPendingPreset.exchange (false))
    {
      ChainPreset p = pendingPreset;
      graphRebuilder.applyPresetTopology (p, true);
      if (onChainRebuilt) onChainRebuilt();
    }
  });

  // Wire LLM callback: detect topology change vs knob-only tweak.
  // Lives on Processor (not Editor), so presets apply even with UI closed.
  llmCoordinator.onPresetGenerated = [this](const ChainPreset &p) {
    DBG ("onPresetGenerated called: " + juce::String (p.numPlugins)
         + " plugins, chainNodes=" + juce::String ((int) chainNodes.size()));

    bool topologyChanged = (p.numPlugins != static_cast<int>(chainNodes.size()));

    if (!topologyChanged) {
      for (int i = 0; i < p.numPlugins; ++i) {
        auto* proc = chainNodes[static_cast<size_t>(i)] != nullptr
                         ? dynamic_cast<juce::AudioPluginInstance*>(chainNodes[static_cast<size_t>(i)]->getProcessor())
                         : nullptr;
        if (proc == nullptr || proc->getPluginDescription().uniqueId != p.plugins[static_cast<size_t>(i)].uid) {
          topologyChanged = true;
          break;
        }
      }
    }

    bool hasBypassActions = false;
    for (int i = 0; i < p.numPlugins; ++i)
    {
      uint8_t act = p.slotAction[static_cast<size_t> (i)];
      if (act == SlotActions::Bypass || act == SlotActions::Enable)
      {
        hasBypassActions = true;
        break;
      }
    }

    DBG ("topologyChanged=" + juce::String (topologyChanged ? "YES" : "NO")
         + " bypassActions=" + juce::String (hasBypassActions ? "YES" : "NO"));

    if (topologyChanged) {
      juce::MessageManager::callAsync([this, p]() {
        DBG ("applyPresetTopology called from async");
        saveStateToUndo();
        graphRebuilder.applyPresetTopology(p);
      });
    } else {
      if (hasBypassActions)
      {
        juce::MessageManager::callAsync([this, p]() {
          graphRebuilder.applyBypassChanges(p);
        });
      }

      pushPresetToDSP(p);
      juce::MessageManager::callAsync([this]() {
        saveStateToUndo();
      });
      juce::Timer::callAfterDelay (250, [this]() {
        intentTracker.snapshotAIState (chainNodes);
      });
    }
  };

  // ── Pre-allocate 1024 automation slots ──
  for (int i = 0; i < kAutomationPoolSize; ++i)
  {
    auto* slot = new DynamicSlotParameter (i);
    slot->addListener (this);
    paramSlots[static_cast<size_t> (i)] = slot;
    addParameter (slot);
  }

  llmCoordinator.startThread();

  // ── Telemetry bridge: background ping every 15s ──
  telemetryBridge.onTick = [this]() {
    intentTracker.detectUserTouches (chainNodes, currentPreset);
    auto json = StudioTelemetryBridge::buildPayload (
        instanceId, trackName, projectScope, currentPreset, currentMetrics);
    telemetryBridge.sendPayloadAsync (std::move (json));
  };
  telemetryBridge.startPinging (15000);

  // ── Backend health checker: poll /health every 10s ──
  healthChecker.startChecking (10000);
}

ChainPromptAudioProcessor::~ChainPromptAudioProcessor() {
  llmCoordinator.onPresetGenerated = nullptr;
  llmCoordinator.onChatMessage = nullptr;
  onChainRebuilt = nullptr;

  pluginCache->removeListener (scanListenerId);

  healthChecker.stopChecking();
  telemetryBridge.stopPinging();
  graphRebuilder.stopTimers();

  llmCoordinator.stopThread(4000);
}

void ChainPromptAudioProcessor::prepareToPlay(double sampleRate,
                                              int samplesPerBlock) {
  mainGraph->enableAllBuses();
  {
    juce::AudioProcessor::BusesLayout graphLayout;
    graphLayout.inputBuses.add (juce::AudioChannelSet::stereo());
    graphLayout.outputBuses.add (juce::AudioChannelSet::stereo());
    mainGraph->setBusesLayout (graphLayout);
  }
  mainGraph->setPlayConfigDetails(getMainBusNumInputChannels(),
                                  getMainBusNumOutputChannels(), sampleRate,
                                  samplesPerBlock);
  mainGraph->prepareToPlay(sampleRate, samplesPerBlock);

  const bool isStereo = (getTotalNumOutputChannels() > 1)
                     || (getMainBusNumOutputChannels() > 1);
  currentMetrics.isStereo.store (isStereo, std::memory_order_relaxed);

  const int numCh = getTotalNumInputChannels();

  captureManager.prepareToPlay (numCh, sampleRate);
  autoGain.prepareToPlay (sampleRate, samplesPerBlock, numCh);

  crossfadeGain = 1.0f;
  crossfadeTarget.store (1.0f, std::memory_order_relaxed);

  // ── 5-Band Spectral Analyzer ──
  bandBuffer.setSize(numCh, samplesPerBlock);

  juce::dsp::ProcessSpec spectralSpec { sampleRate,
                                         static_cast<juce::uint32>(samplesPerBlock),
                                         static_cast<juce::uint32>(numCh) };

  *spectralFilters[0].state = *juce::dsp::IIR::Coefficients<float>::makeLowPass(sampleRate, 60.0f);
  *spectralFilters[1].state = *juce::dsp::IIR::Coefficients<float>::makeBandPass(sampleRate, 155.0f, 0.707f);
  *spectralFilters[2].state = *juce::dsp::IIR::Coefficients<float>::makeBandPass(sampleRate, 1125.0f, 0.707f);
  *spectralFilters[3].state = *juce::dsp::IIR::Coefficients<float>::makeBandPass(sampleRate, 5000.0f, 0.707f);
  *spectralFilters[4].state = *juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, 8000.0f);

  for (auto& f : spectralFilters)
  {
    f.prepare(spectralSpec);
    f.reset();
  }

  smoothedSubPct = 0.0f;
  smoothedLowPct = 0.0f;
  smoothedLowMidPct = 0.0f;
  smoothedHighMidPct = 0.0f;
  smoothedAirPct = 0.0f;

  if (chainNodes.empty()) {
    using UK = juce::AudioProcessorGraph::UpdateKind;
    using AudioGraphIOProcessor = juce::AudioProcessorGraph::AudioGraphIOProcessor;

    mainGraph->clear (UK::none);

    audioInputNode  = mainGraph->addNode(
        std::make_unique<AudioGraphIOProcessor>(AudioGraphIOProcessor::audioInputNode),  {}, UK::none);
    audioOutputNode = mainGraph->addNode(
        std::make_unique<AudioGraphIOProcessor>(AudioGraphIOProcessor::audioOutputNode), {}, UK::none);

    if (audioInputNode && audioOutputNode) {
      for (int channel = 0; channel < 2; ++channel) {
        mainGraph->addConnection(
            {{audioInputNode->nodeID, channel}, {audioOutputNode->nodeID, channel}}, UK::none);
      }
    }

    mainGraph->prepareToPlay (sampleRate, samplesPerBlock);
  }
}

void ChainPromptAudioProcessor::releaseResources() {
  mainGraph->releaseResources();
}

// ─────────────────────────────────────────────────────────────────────────────
// handleAsyncUpdate: delegates to OfflineAudioAnalyzer + AudioCaptureManager
// ─────────────────────────────────────────────────────────────────────────────
void ChainPromptAudioProcessor::handleAsyncUpdate()
{
  if (captureManager.captureState.load (std::memory_order_acquire) != CaptureState::Done)
    return;

  const int totalSamples = captureManager.captureTotalSamples.load();
  const double sr = getSampleRate() > 0 ? getSampleRate() : 44100.0;

  offlineAnalyzer.analyze (captureManager.captureBuffer, totalSamples, sr, currentMetrics);

  {
    auto& fm = captureManager.frozenMetrics;
    fm.rmsDB        = currentMetrics.rmsDB.load (std::memory_order_relaxed);
    fm.peakDB       = currentMetrics.peakDB.load (std::memory_order_relaxed);
    fm.crestFactor  = currentMetrics.crestFactor.load (std::memory_order_relaxed);
    fm.zcr          = currentMetrics.zcr.load (std::memory_order_relaxed);
    fm.bpm          = currentMetrics.bpm.load (std::memory_order_relaxed);
    fm.subPct       = currentMetrics.subPct.load (std::memory_order_relaxed);
    fm.lowPct       = currentMetrics.lowPct.load (std::memory_order_relaxed);
    fm.lowMidPct    = currentMetrics.lowMidPct.load (std::memory_order_relaxed);
    fm.highMidPct   = currentMetrics.highMidPct.load (std::memory_order_relaxed);
    fm.airPct       = currentMetrics.airPct.load (std::memory_order_relaxed);
    fm.isStereo     = currentMetrics.isStereo.load (std::memory_order_relaxed);
    fm.spectrum31Ready = currentMetrics.spectrum31Ready.load (std::memory_order_relaxed);
    for (int i = 0; i < 31; ++i)
      fm.spectrum31[static_cast<size_t> (i)] = currentMetrics.spectrum31[static_cast<size_t> (i)].load (std::memory_order_relaxed);
    fm.mdTserDb     = currentMetrics.mdTserDb.load (std::memory_order_relaxed);
    fm.spectralFlux = currentMetrics.spectralFlux.load (std::memory_order_relaxed);
    fm.oeHerDb      = currentMetrics.oeHerDb.load (std::memory_order_relaxed);
    fm.isTonal      = currentMetrics.isTonal.load (std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i)
      fm.bwIcc[static_cast<size_t> (i)] = currentMetrics.bwIcc[static_cast<size_t> (i)].load (std::memory_order_relaxed);
    fm.lrBalanceDb  = currentMetrics.lrBalanceDb.load (std::memory_order_relaxed);
  }

  captureManager.saveCaptureToTempFile (sr, instanceId);

  juce::Logger::writeToLog ("Capture complete: RMS=" + juce::String (currentMetrics.rmsDB.load (std::memory_order_relaxed), 1)
      + " Peak=" + juce::String (currentMetrics.peakDB.load (std::memory_order_relaxed), 1)
      + " Crest=" + juce::String (currentMetrics.crestFactor.load (std::memory_order_relaxed), 1)
      + " ZCR=" + juce::String (currentMetrics.zcr.load (std::memory_order_relaxed), 3)
      + " Base64=" + juce::String (captureManager.capturedAudioBase64.length()) + " chars");
}

// ─────────────────────────────────────────────────────────────────────────────
// isBusesLayoutSupported — stereo/mono only, NO sidechain
// ─────────────────────────────────────────────────────────────────────────────
bool ChainPromptAudioProcessor::isBusesLayoutSupported(
    const BusesLayout &layouts) const {
  if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
      layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
    return false;

  if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
    return false;

  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// processBlock
// ─────────────────────────────────────────────────────────────────────────────
void ChainPromptAudioProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                             juce::MidiBuffer &midiMessages) {
  if (getTotalNumInputChannels() == 0 || buffer.getNumChannels() == 0)
    return;

  juce::ScopedNoDenormals noDenormals;

  auto totalNumInputChannels = getTotalNumInputChannels();
  auto totalNumOutputChannels = getTotalNumOutputChannels();
  for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
    buffer.clear(i, 0, buffer.getNumSamples());

  const bool capturePostGraph = activeChainSize.load (std::memory_order_relaxed) > 0;

  if (! capturePostGraph)
  {
    if (captureManager.processBlock (buffer, totalNumInputChannels))
      triggerAsyncUpdate();
  }

  // ── Tier 1 DSP Analysis: O(N) single-pass on DRY input ──
  {
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = juce::jmin (totalNumInputChannels, buffer.getNumChannels());

    float sumSquares = 0.0f;
    float peak       = 0.0f;
    int   zeroCrossings = 0;

    for (int ch = 0; ch < numChannels; ++ch)
    {
      const float* data = buffer.getReadPointer (ch);
      float prev = data[0];

      for (int i = 0; i < numSamples; ++i)
      {
        const float s = data[i];
        sumSquares += s * s;

        const float absS = std::abs (s);
        if (absS > peak)
          peak = absS;

        if ((s > 0.0f && prev < 0.0f) || (s < 0.0f && prev > 0.0f))
          ++zeroCrossings;

        prev = s;
      }
    }

    const float totalSamples = static_cast<float> (numSamples * juce::jmax (numChannels, 1));
    const float rmsLinear = std::sqrt (sumSquares / juce::jmax (totalSamples, 1.0f));

    float rmsDB  = juce::Decibels::gainToDecibels (rmsLinear, -100.0f);
    float peakDB = juce::Decibels::gainToDecibels (peak,      -100.0f);
    float crest  = peakDB - rmsDB;
    float zcrNorm = static_cast<float> (zeroCrossings) / juce::jmax (totalSamples, 1.0f);

    if (std::isnan (rmsDB)  || std::isinf (rmsDB))   rmsDB  = -100.0f;
    if (std::isnan (peakDB) || std::isinf (peakDB))   peakDB = -100.0f;
    if (std::isnan (crest)  || std::isinf (crest))     crest  = 0.0f;
    if (std::isnan (zcrNorm) || std::isinf (zcrNorm))  zcrNorm = 0.0f;

    rmsDB  = juce::jmax (rmsDB,  -100.0f);
    peakDB = juce::jmax (peakDB, -100.0f);
    crest  = juce::jlimit (0.0f, 60.0f, crest);
    zcrNorm = juce::jlimit (0.0f, 1.0f, zcrNorm);

    constexpr float alpha = 0.05f;
    smoothedRmsDB  += alpha * (rmsDB  - smoothedRmsDB);
    smoothedPeakDB += alpha * (peakDB - smoothedPeakDB);
    smoothedCrest  += alpha * (crest  - smoothedCrest);
    smoothedZcr    += alpha * (zcrNorm - smoothedZcr);

    if (std::isnan (smoothedRmsDB))  smoothedRmsDB  = -100.0f;
    if (std::isnan (smoothedPeakDB)) smoothedPeakDB = -100.0f;
    if (std::isnan (smoothedCrest))  smoothedCrest  = 0.0f;
    if (std::isnan (smoothedZcr))    smoothedZcr    = 0.0f;

    constexpr float releaseAlpha = 0.001f;

    auto envelopeFollow = [](float rawVal, float currentVal, float relAlpha) -> float {
      if (rawVal > currentVal)
        return rawVal;
      return currentVal * (1.0f - relAlpha) + rawVal * relAlpha;
    };

    float envRmsDB  = envelopeFollow(smoothedRmsDB,  currentMetrics.rmsDB.load(std::memory_order_relaxed),  releaseAlpha);
    float envPeakDB = envelopeFollow(smoothedPeakDB, currentMetrics.peakDB.load(std::memory_order_relaxed), releaseAlpha);
    float envCrest  = envelopeFollow(smoothedCrest,   currentMetrics.crestFactor.load(std::memory_order_relaxed), releaseAlpha);

    envRmsDB  = juce::jmax(envRmsDB,  -100.0f);
    envPeakDB = juce::jmax(envPeakDB, -100.0f);
    envCrest  = juce::jlimit(0.0f, 60.0f, envCrest);

    currentMetrics.rmsDB.store  (envRmsDB,       std::memory_order_relaxed);
    currentMetrics.peakDB.store (envPeakDB,      std::memory_order_relaxed);
    currentMetrics.crestFactor.store (envCrest,   std::memory_order_relaxed);
    currentMetrics.zcr.store    (smoothedZcr,    std::memory_order_relaxed);

    if (auto* ph = getPlayHead())
    {
      if (auto posInfo = ph->getPosition())
      {
        if (auto bpmOpt = posInfo->getBpm())
          currentMetrics.bpm.store (*bpmOpt, std::memory_order_relaxed);
      }
    }
  }

  // ── 5-Band Spectral Analysis ──
  {
    const int spNumSamples  = buffer.getNumSamples();
    const int spNumChannels = juce::jmin(getTotalNumInputChannels(), buffer.getNumChannels());

    float bandRms[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

    for (int band = 0; band < 5; ++band)
    {
      for (int ch = 0; ch < spNumChannels; ++ch)
        bandBuffer.copyFrom(ch, 0, buffer, ch, 0, spNumSamples);

      juce::dsp::AudioBlock<float> block(bandBuffer.getArrayOfWritePointers(),
                                          static_cast<size_t>(spNumChannels),
                                          static_cast<size_t>(spNumSamples));
      juce::dsp::ProcessContextReplacing<float> ctx(block);
      spectralFilters[static_cast<size_t>(band)].process(ctx);

      float sumSq = 0.0f;
      for (int ch = 0; ch < spNumChannels; ++ch)
      {
        const float* data = bandBuffer.getReadPointer(ch);
        for (int i = 0; i < spNumSamples; ++i)
          sumSq += data[i] * data[i];
      }
      bandRms[band] = std::sqrt(sumSq / juce::jmax(static_cast<float>(spNumSamples * spNumChannels), 1.0f));
    }

    float totalSpectralRms = bandRms[0] + bandRms[1] + bandRms[2] + bandRms[3] + bandRms[4] + 1e-9f;

    float pct[5];
    for (int i = 0; i < 5; ++i)
      pct[i] = (bandRms[i] / totalSpectralRms) * 100.0f;

    constexpr float spectralRelease = 0.001f;

    auto spectralEnvelope = [](float raw, float current, float rel) -> float {
      if (raw > current)
        return raw;
      return current * (1.0f - rel) + raw * rel;
    };

    smoothedSubPct     = spectralEnvelope(pct[0], smoothedSubPct,     spectralRelease);
    smoothedLowPct     = spectralEnvelope(pct[1], smoothedLowPct,     spectralRelease);
    smoothedLowMidPct  = spectralEnvelope(pct[2], smoothedLowMidPct,  spectralRelease);
    smoothedHighMidPct = spectralEnvelope(pct[3], smoothedHighMidPct, spectralRelease);
    smoothedAirPct     = spectralEnvelope(pct[4], smoothedAirPct,     spectralRelease);

    currentMetrics.subPct.store    (smoothedSubPct,     std::memory_order_relaxed);
    currentMetrics.lowPct.store    (smoothedLowPct,     std::memory_order_relaxed);
    currentMetrics.lowMidPct.store (smoothedLowMidPct,  std::memory_order_relaxed);
    currentMetrics.highMidPct.store(smoothedHighMidPct, std::memory_order_relaxed);
    currentMetrics.airPct.store    (smoothedAirPct,     std::memory_order_relaxed);
  }

  // ── Perceptual Auto-Gain ──
  const int numChannels = buffer.getNumChannels();
  const int numSamples  = buffer.getNumSamples();

  autoGain.measureDryInput (buffer, numSamples);

  float diagPre = 0.0f;
  for (int ch = 0; ch < numChannels; ++ch)
  {
    const float* d = buffer.getReadPointer(ch);
    for (int s = 0; s < numSamples; ++s)
      diagPre += d[s] * d[s];
  }
  diagPre = std::sqrt(diagPre / juce::jmax(static_cast<float>(numSamples * numChannels), 1.0f));

  bool graphBypassed = isRebuildingGraph.load(std::memory_order_acquire);
  if (!graphBypassed)
    mainGraph->processBlock(buffer, midiMessages);

  float diagPost = 0.0f;
  for (int ch = 0; ch < numChannels; ++ch)
  {
    const float* d = buffer.getReadPointer(ch);
    for (int s = 0; s < numSamples; ++s)
      diagPost += d[s] * d[s];
  }
  diagPost = std::sqrt(diagPost / juce::jmax(static_cast<float>(numSamples * numChannels), 1.0f));

  currentMetrics.diagPreRms.store(diagPre, std::memory_order_relaxed);
  currentMetrics.diagPostRms.store(diagPost, std::memory_order_relaxed);
  currentMetrics.diagGraphBypassed.store(graphBypassed, std::memory_order_relaxed);

  if (capturePostGraph)
  {
    if (captureManager.processBlock (buffer, totalNumInputChannels))
      triggerAsyncUpdate();
  }

  autoGain.measureWetOutput (buffer, numSamples);
  autoGain.applyGain (buffer, numSamples, autoGainEnabled.load (std::memory_order_relaxed));

  currentMetrics.diagAutoGain.store(autoGain.getTargetGain(), std::memory_order_relaxed);

  // ── Crossfade ramp ──
  {
    const float target = crossfadeTarget.load (std::memory_order_relaxed);
    for (int i = 0; i < numSamples; ++i)
    {
      crossfadeGain += 0.003f * (target - crossfadeGain);
      if (crossfadeGain < 0.0001f && target == 0.0f)
        crossfadeGain = 0.0f;
      else if (crossfadeGain > 0.9999f && target == 1.0f)
        crossfadeGain = 1.0f;

      for (int ch = 0; ch < numChannels; ++ch)
        buffer.getWritePointer (ch)[i] *= crossfadeGain;
    }
  }

  // Poll lock-free FIFO for new presets from LLM thread
  int start1, size1, start2, size2;
  presetFifo.prepareToRead(1, start1, size1, start2, size2);

  if (size1 > 0) {
    const auto &preset = presetBuffer[static_cast<size_t>(start1)];
    int totalParamsSet = 0;

    for (int i = 0; i < preset.numPlugins; ++i) {
      if (i < static_cast<int>(chainNodes.size()) && chainNodes[static_cast<size_t>(i)] != nullptr) {
        if (auto *proc = chainNodes[static_cast<size_t>(i)]->getProcessor()) {
          auto &params = proc->getParameters();

          for (int p = 0; p < preset.plugins[static_cast<size_t>(i)].numParams; ++p) {
            const auto &paramSetting = preset.plugins[static_cast<size_t>(i)].params[static_cast<size_t>(p)];
            if (paramSetting.index <= 0)
              continue;
            if (paramSetting.index < params.size()) {
              if (auto *param = params[paramSetting.index]) {
                param->setValueNotifyingHost(paramSetting.value);
                ++totalParamsSet;
              }
            }
          }
        }
      }
    }

    currentMetrics.diagFifoApplied.fetch_add(1, std::memory_order_relaxed);
    currentMetrics.diagParamsSet.store(totalParamsSet, std::memory_order_relaxed);

    presetFifo.finishedRead(size1 + size2);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Delegation: intent tracking
// ─────────────────────────────────────────────────────────────────────────────
void ChainPromptAudioProcessor::detectUserTouches()
{
    intentTracker.detectUserTouches (chainNodes, currentPreset);
}

// ─────────────────────────────────────────────────────────────────────────────
// Delegation: graph topology
// ─────────────────────────────────────────────────────────────────────────────
void ChainPromptAudioProcessor::loadTestChain() {
  ChainPreset defaultPreset;
  defaultPreset.numPlugins = 1;
  defaultPreset.plugins[0].uid = -743038935;
  defaultPreset.plugins[0].numParams = 0;

  graphRebuilder.applyPresetTopology(defaultPreset);
}

void ChainPromptAudioProcessor::applyPresetTopology(const ChainPreset &preset, bool isFromDAWLoad) {
  graphRebuilder.applyPresetTopology (preset, isFromDAWLoad);
}

void ChainPromptAudioProcessor::rewireGraphConnections() {
  graphRebuilder.rewireGraphConnections();
}

void ChainPromptAudioProcessor::applyBypassChanges(const ChainPreset& preset) {
  graphRebuilder.applyBypassChanges (preset);
}

void ChainPromptAudioProcessor::setPluginBypassed(int index, bool shouldBypass) {
  if (index < 0 || index >= static_cast<int>(chainNodes.size()))
    return;

  currentPreset.bypassed[static_cast<size_t>(index)] = shouldBypass;
  currentPreset.userTouched[static_cast<size_t>(index)] = true;
  graphRebuilder.rewireGraphConnections();
}

// ─────────────────────────────────────────────────────────────────────────────
// Automation forwarding
// ─────────────────────────────────────────────────────────────────────────────
void ChainPromptAudioProcessor::parameterValueChanged (int parameterIndex, float newValue)
{
    if (parameterIndex < 0 || parameterIndex >= kAutomationPoolSize)
        return;

    auto* target = paramSlots[static_cast<size_t> (parameterIndex)]
                       ->targetParam.load (std::memory_order_acquire);

    if (target != nullptr)
        target->setValueNotifyingHost (newValue);
}

void ChainPromptAudioProcessor::parameterGestureChanged (int parameterIndex, bool gestureIsStarting)
{
    if (parameterIndex < 0 || parameterIndex >= kAutomationPoolSize)
        return;

    auto* target = paramSlots[static_cast<size_t> (parameterIndex)]
                       ->targetParam.load (std::memory_order_acquire);

    if (target != nullptr)
    {
        if (gestureIsStarting)
            target->beginChangeGesture();
        else
            target->endChangeGesture();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// pushPresetToDSP
// ─────────────────────────────────────────────────────────────────────────────
void ChainPromptAudioProcessor::pushPresetToDSP(const ChainPreset &preset) {
  jassert (preset.numPlugins == currentPreset.numPlugins);
  for (int i = 0; i < preset.numPlugins; ++i)
  {
    const auto& incoming = preset.plugins[static_cast<size_t> (i)];
    if (incoming.numParams > 0)
    {
      currentPreset.plugins[static_cast<size_t> (i)].params = incoming.params;
      currentPreset.plugins[static_cast<size_t> (i)].numParams = incoming.numParams;
    }
  }

  ParamPreset pp = currentPreset.toParamPreset();

  int start1, size1, start2, size2;
  presetFifo.prepareToWrite(1, start1, size1, start2, size2);

  if (size1 > 0) {
    presetBuffer[static_cast<size_t>(start1)] = pp;
    presetFifo.finishedWrite(size1 + size2);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Undo/Redo (uses ChainStateSerializer for state capture)
// ─────────────────────────────────────────────────────────────────────────────
void ChainPromptAudioProcessor::saveStateToUndo()
{
    ChainStateSerializer::capturePluginStates (chainNodes, currentPreset);

    undoStack.push_back (currentPreset);

    if (static_cast<int> (undoStack.size()) > kMaxUndoHistory)
        undoStack.erase (undoStack.begin());

    redoStack.clear();

    juce::Logger::writeToLog ("Undo: saved state (stack=" + juce::String (static_cast<int> (undoStack.size())) + ")");
}

void ChainPromptAudioProcessor::undo()
{
    if (undoStack.empty())
        return;

    ChainStateSerializer::capturePluginStates (chainNodes, currentPreset);
    redoStack.push_back (currentPreset);

    ChainPreset target = undoStack.back();
    undoStack.pop_back();

    juce::Logger::writeToLog ("Undo: restoring (undo=" + juce::String (static_cast<int> (undoStack.size()))
        + " redo=" + juce::String (static_cast<int> (redoStack.size())) + ")");

    graphRebuilder.applyPresetTopology (target, true);
}

void ChainPromptAudioProcessor::redo()
{
    if (redoStack.empty())
        return;

    ChainStateSerializer::capturePluginStates (chainNodes, currentPreset);
    undoStack.push_back (currentPreset);

    ChainPreset target = redoStack.back();
    redoStack.pop_back();

    juce::Logger::writeToLog ("Redo: restoring (undo=" + juce::String (static_cast<int> (undoStack.size()))
        + " redo=" + juce::String (static_cast<int> (redoStack.size())) + ")");

    graphRebuilder.applyPresetTopology (target, true);
}

// ─────────────────────────────────────────────────────────────────────────────
// Boilerplate
// ─────────────────────────────────────────────────────────────────────────────
juce::AudioProcessorEditor *ChainPromptAudioProcessor::createEditor() {
  return new ChainPromptAudioProcessorEditor(*this);
}

bool ChainPromptAudioProcessor::hasEditor() const { return true; }

const juce::String ChainPromptAudioProcessor::getName() const {
  return JucePlugin_Name;
}

bool ChainPromptAudioProcessor::acceptsMidi() const { return false; }

bool ChainPromptAudioProcessor::producesMidi() const { return false; }

bool ChainPromptAudioProcessor::isMidiEffect() const { return false; }

double ChainPromptAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int ChainPromptAudioProcessor::getNumPrograms() { return 1; }

int ChainPromptAudioProcessor::getCurrentProgram() { return 0; }

void ChainPromptAudioProcessor::setCurrentProgram(int index) {}

const juce::String ChainPromptAudioProcessor::getProgramName(int index) {
  return {};
}

void ChainPromptAudioProcessor::changeProgramName(int index,
                                                  const juce::String &newName) {
}

// ─────────────────────────────────────────────────────────────────────────────
// DAW State Save/Load (delegates to ChainStateSerializer)
// ─────────────────────────────────────────────────────────────────────────────
void ChainPromptAudioProcessor::updateTrackProperties(const TrackProperties& properties) {
  if (properties.name.has_value() && properties.name->isNotEmpty())
    trackName = *properties.name;
  DBG ("updateTrackProperties: track='" + trackName + "' instanceId=" + instanceId);
}

void ChainPromptAudioProcessor::getStateInformation(
    juce::MemoryBlock &destData) {
  ChainStateSerializer::serialize (
      destData, instanceId, trackName, nextSlotId,
      chainNodes, currentPreset,
      hasPendingPreset.load(), pendingPreset,
      llmCoordinator);
}

void ChainPromptAudioProcessor::setStateInformation(const void *data,
                                                    int sizeInBytes) {
  auto state = ChainStateSerializer::deserialize (data, sizeInBytes);
  if (! state.valid)
    return;

  // Restore identity
  if (state.instanceId.isNotEmpty())
    instanceId = state.instanceId;
  if (state.trackName.isNotEmpty())
    trackName = state.trackName;
  if (state.nextSlotId > 0)
    nextSlotId = state.nextSlotId;

  DBG ("setStateInformation: restored instanceId=" + instanceId + " track='" + trackName
       + "' nextSlotId=" + juce::String (nextSlotId));

  // Restore chat history
  llmCoordinator.restoreChatHistory (std::move (state.chatHistory));

  // NOTE: projectScope NOT restored — always use live PID-based scope.

  currentPreset = state.preset;

  if (pluginCache->isReady()) {
    juce::MessageManager::callAsync([this, preset = state.preset]() {
      graphRebuilder.applyPresetTopology(preset, true);
    });
  } else {
    pendingPreset = state.preset;
    hasPendingPreset.store(true);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Plugin instance factory
// ─────────────────────────────────────────────────────────────────────────────
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new ChainPromptAudioProcessor();
}
