#pragma once

/**
 * ChainGraphRebuilder — Topology apply/rebuild, node recycling, wiring, bypass.
 *
 * Extracted from ChainPromptAudioProcessor (Sprint 11: Great Decoupling, Step 5).
 * Owns the rebuild state machine (crossfade → suspend → rebuild → fade-in) and
 * the ModalDismissTimer. Forward-declares processor; implementations in .cpp.
 *
 * Key contract changes in this extraction:
 *   - Sidechain bus DELETED from constructor + isBusesLayoutSupported
 *   - Mono/stereo bus policy made explicit: stereo-only graph, mono plugins get
 *     stereo layout forced (with mono→stereo fan-out wiring as fallback)
 *   - Session scope: explicit settable token (no longer hardcoded PID)
 */

#include <JuceHeader.h>
#include "PresetData.h"
#include "SlotStateOps.h"

class ChainPromptAudioProcessor;
class AIIntentTracker;
class AutomationPoolRouter;

class ChainGraphRebuilder
{
public:
    explicit ChainGraphRebuilder (ChainPromptAudioProcessor& p);

    /** Initiate topology change: crossfade out → schedule rebuild. Message thread only. */
    void applyPresetTopology (const ChainPreset& preset, bool isFromDAWLoad = false);

    /** Actual graph rebuild (called by RebuildTimer after fade-out). Message thread only. */
    void performRebuild();

    /** Wire source→dest with stereo/mono handling. */
    void connectNodes (juce::AudioProcessorGraph::Node::Ptr source,
                       juce::AudioProcessorGraph::Node::Ptr dest);

    /** Rewire graph connections respecting bypass state. */
    void rewireGraphConnections();

    /** Apply bypass/enable actions from server without topology rebuild. */
    void applyBypassChanges (const ChainPreset& preset);

    /** Apply LLM-generated parameters to a single graph node. */
    static void applyParamsToNode (juce::AudioProcessorGraph::Node::Ptr node,
                                   const PluginConfig& pc, const juce::String& label);

    void stopTimers()
    {
        rebuildTimer.stopTimer();
        modalGuard.stopTimer();
    }

private:
    ChainPromptAudioProcessor& processor;

    // ── Rebuild State Machine ──
    ChainPreset rebuildPreset;
    bool rebuildIsFromDAWLoad = false;

    struct RebuildTimer : public juce::Timer
    {
        ChainGraphRebuilder* owner = nullptr;
        void timerCallback() override
        {
            stopTimer();
            if (owner != nullptr)
                owner->performRebuild();
        }
    };
    RebuildTimer rebuildTimer;

    // ── Modal Dismissal Guard (DISABLED — kept for future GUI management) ──
    struct ModalDismissTimer : public juce::Timer
    {
        int ticksRemaining = 0;
        int intervalMs = 500;

        void start (int ticks, int interval = 500)
        {
            ticksRemaining = ticks;
            intervalMs = interval;
            startTimer (intervalMs);
        }

        void timerCallback() override
        {
            for (int safety = 0; safety < 20; ++safety)
            {
                auto* modal = juce::Component::getCurrentlyModalComponent();
                if (modal == nullptr) break;

                juce::Logger::writeToLog ("ModalDismissTimer: killing stray modal: "
                    + modal->getName());
                modal->exitModalState (0);

                if (juce::Component::getCurrentlyModalComponent() == modal)
                {
                    juce::Logger::writeToLog ("ModalDismissTimer: exitModalState had no effect, force-hiding.");
                    modal->setVisible (false);
                    break;
                }
            }
            if (--ticksRemaining <= 0)
                stopTimer();
        }
    };
    ModalDismissTimer modalGuard;
};
