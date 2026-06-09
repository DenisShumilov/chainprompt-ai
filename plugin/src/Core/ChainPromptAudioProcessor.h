#pragma once

#include <JuceHeader.h>
#include "PresetData.h"
#include "AudioMetrics.h"
#include "LLMCoordinator.h"
#include "GlobalPluginCache.h"
#include "AudioCaptureManager.h"
#include "OfflineAudioAnalyzer.h"
#include "PerceptualAutoGain.h"
#include "DynamicSlotParameter.h"
#include "SlotStateOps.h"
#include "AIIntentTracker.h"
#include "AutomationPoolRouter.h"
#include "StudioTelemetryBridge.h"
#include "BackendHealthChecker.h"
#include "ChainGraphRebuilder.h"

class ChainPromptAudioProcessorEditor;

class ChainPromptAudioProcessor : public juce::AudioProcessor,
                                  public juce::AsyncUpdater,
                                  public juce::AudioProcessorParameter::Listener {
public:
  ChainPromptAudioProcessor();
  ~ChainPromptAudioProcessor() override;

  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override;
  void handleAsyncUpdate() override;

  bool isBusesLayoutSupported(const BusesLayout &layouts) const override;

  void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;

  juce::AudioProcessorEditor *createEditor() override;
  bool hasEditor() const override;

  const juce::String getName() const override;

  bool acceptsMidi() const override;
  bool producesMidi() const override;
  bool isMidiEffect() const override;
  double getTailLengthSeconds() const override;

  int getNumPrograms() override;
  int getCurrentProgram() override;
  void setCurrentProgram(int index) override;
  const juce::String getProgramName(int index) override;
  void changeProgramName(int index, const juce::String &newName) override;

  void getStateInformation(juce::MemoryBlock &destData) override;
  void setStateInformation(const void *data, int sizeInBytes) override;
  void updateTrackProperties(const TrackProperties& properties) override;

  // ── AudioProcessorParameter::Listener (automation forwarding) ──
  void parameterValueChanged (int parameterIndex, float newValue) override;
  void parameterGestureChanged (int parameterIndex, bool gestureIsStarting) override;

  void loadTestChain();

  void pushPresetToDSP(const ChainPreset &preset);
  void applyPresetTopology(const ChainPreset &preset, bool isFromDAWLoad = false);
  void setPluginBypassed(int index, bool shouldBypass);
  void rewireGraphConnections();

  /** Apply bypass/enable actions from server without topology rebuild.
      Delegates to graphRebuilder. */
  void applyBypassChanges(const ChainPreset& preset);

  // ── Undo/Redo: ChainPreset history stack ──
  void saveStateToUndo();
  void undo();
  void redo();
  bool canUndo() const noexcept { return ! undoStack.empty(); }
  bool canRedo() const noexcept { return ! redoStack.empty(); }

  /** Detect user-touched slots. Delegates to intentTracker. */
  void detectUserTouches();

  bool isPluginCacheReady() const noexcept { return pluginCache->isReady(); }
  GlobalPluginCache& getPluginCache() noexcept { return *pluginCache; }

  // Dynamic chain nodes (public for Editor sidebar access)
  std::vector<juce::AudioProcessorGraph::Node::Ptr> chainNodes;

  // Callback: fired on Message Thread after chain topology is rebuilt
  std::function<void()> onChainRebuilt;

  LLMCoordinator llmCoordinator;

  // A/B Loudness Match: atomic toggle readable from UI
  std::atomic<bool> autoGainEnabled { true };

  // ── Subsystems (public for Editor access) ──
  AudioCaptureManager  captureManager;
  OfflineAudioAnalyzer offlineAnalyzer;
  PerceptualAutoGain   autoGain;

  // ── Extracted modules (Sprint 11: Great Decoupling) ──
  AIIntentTracker      intentTracker;
  AutomationPoolRouter automationRouter;
  StudioTelemetryBridge telemetryBridge;
  BackendHealthChecker  healthChecker;

  // Tier 1 audio metrics (written on audio thread, read on message thread)
  AudioMetrics currentMetrics;

  // Last applied preset (cached for DAW state saving)
  ChainPreset currentPreset;

  // Studio Memory: unique instance identity
  juce::String instanceId;
  juce::String trackName { "Unknown Track" };

  // Project/session scope: explicit token for cross-track query scoping.
  // Default: PID-based (all instances in same DAW process share scope).
  // Future: Master Mode can override with real project hash.
  juce::String projectScope;

private:
  friend class ChainGraphRebuilder;

  // Global shared plugin cache
  juce::SharedResourcePointer<GlobalPluginCache> pluginCache;

  std::unique_ptr<juce::AudioProcessorGraph> mainGraph;

  juce::AudioProcessorGraph::Node::Ptr audioInputNode;
  juce::AudioProcessorGraph::Node::Ptr audioOutputNode;

  // Thread-safety guard: audio thread bypasses graph during rebuild
  std::atomic<bool> isRebuildingGraph { false };

  // Number of active plugins in chain (atomic for audio-thread-safe reads)
  std::atomic<int> activeChainSize { 0 };

  // ── Crossfade: smooth fade-out/in around topology changes ──
  std::atomic<float> crossfadeTarget { 1.0f };
  float crossfadeGain = 1.0f;  // audio thread only

  // Deferred state loading
  ChainPreset pendingPreset;
  std::atomic<bool> hasPendingPreset { false };

  // Tier 1 smoothing state (audio thread only)
  float smoothedRmsDB   = -100.0f;
  float smoothedPeakDB  = -100.0f;
  float smoothedCrest   = 0.0f;
  float smoothedZcr     = 0.0f;

  // ── 5-Band Spectral Analyzer (IIR filter bank) ──
  using SpectralFilterType = juce::dsp::ProcessorDuplicator<
      juce::dsp::IIR::Filter<float>,
      juce::dsp::IIR::Coefficients<float>>;
  std::array<SpectralFilterType, 5> spectralFilters;
  juce::AudioBuffer<float> bandBuffer;

  float smoothedSubPct     = 0.0f;
  float smoothedLowPct     = 0.0f;
  float smoothedLowMidPct  = 0.0f;
  float smoothedHighMidPct = 0.0f;
  float smoothedAirPct     = 0.0f;

  // Lock-free FIFO for LLM thread -> audio thread preset transfer
  juce::AbstractFifo presetFifo { 16 };
  std::array<ParamPreset, 16> presetBuffer;

  // ── Undo/Redo history stacks (message thread only) ──
  static constexpr int kMaxUndoHistory = 15;
  std::vector<ChainPreset> undoStack;
  std::vector<ChainPreset> redoStack;

  // ── Dynamic Automation Pool (1024 pre-allocated slots) ──
  std::array<DynamicSlotParameter*, kAutomationPoolSize> paramSlots {};

  // ── Stable Per-Slot Identity (monotonic counter) ──
  int nextSlotId = 1;

  // ── Listener ID for GlobalPluginCache scan-complete callback ──
  int scanListenerId = 0;

  // ── Graph Rebuilder (must be last — constructor takes *this) ──
  ChainGraphRebuilder graphRebuilder;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainPromptAudioProcessor)
};
