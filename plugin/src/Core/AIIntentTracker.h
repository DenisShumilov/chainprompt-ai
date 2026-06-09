#pragma once

/**
 * AIIntentTracker — Snapshot & detect user modifications to AI-applied presets.
 *
 * Extracted from ChainPromptAudioProcessor (Sprint 11: Great Decoupling, Step 2).
 * Owns the AI parameter snapshot array. All methods are message-thread-only.
 *
 * Usage:
 *   tracker.snapshotAIState(chainNodes);          // after AI apply
 *   tracker.detectUserTouches(chainNodes, preset); // before next send / ping
 */

#include <JuceHeader.h>
#include "PresetData.h"
#include <array>
#include <cmath>

class AIIntentTracker
{
public:
    /** Snapshot current hosted plugin parameters after AI preset apply.
        Called at end of performRebuild() and after pushPresetToDSP(). */
    void snapshotAIState (
        const std::vector<juce::AudioProcessorGraph::Node::Ptr>& chainNodes)
    {
        for (size_t i = 0; i < static_cast<size_t> (kMaxPluginsInChain); ++i)
        {
            aiSnapshot[i] = {};

            if (i >= chainNodes.size() || chainNodes[i] == nullptr)
                continue;

            auto* proc = chainNodes[i]->getProcessor();
            if (proc == nullptr)
                continue;

            auto& params = proc->getParameters();
            int count = juce::jmin (params.size(), kMaxParamsPerPlugin);

            for (int p = 0; p < count; ++p)
            {
                if (auto* param = params[p])
                    aiSnapshot[i].values[static_cast<size_t> (p)] = param->getValue();
            }
            aiSnapshot[i].numParams = count;
            aiSnapshot[i].valid = true;
        }

        juce::Logger::writeToLog ("AI Snapshot: captured "
            + juce::String (static_cast<int> (chainNodes.size())) + " slots");
    }

    /** Detect user-touched slots by comparing current hosted plugin params vs snapshot.
        Call on message thread before sending to server. Updates preset.userTouched[]. */
    void detectUserTouches (
        const std::vector<juce::AudioProcessorGraph::Node::Ptr>& chainNodes,
        ChainPreset& currentPreset)
    {
        constexpr float kTolerance = 0.001f;

        for (size_t i = 0; i < static_cast<size_t> (currentPreset.numPlugins); ++i)
        {
            if (currentPreset.userTouched[i])
                continue;

            if (! aiSnapshot[i].valid)
                continue;

            if (i >= chainNodes.size() || chainNodes[i] == nullptr)
                continue;

            auto* proc = chainNodes[i]->getProcessor();
            if (proc == nullptr)
                continue;

            auto& params = proc->getParameters();
            int count = juce::jmin (params.size(), kMaxParamsPerPlugin);

            for (int p = 0; p < count; ++p)
            {
                if (auto* param = params[p])
                {
                    float current = param->getValue();
                    float snapshot = aiSnapshot[i].values[static_cast<size_t> (p)];
                    if (std::abs (current - snapshot) > kTolerance)
                    {
                        currentPreset.userTouched[i] = true;
                        juce::Logger::writeToLog ("User touch detected: slot["
                            + juce::String (static_cast<int> (i))
                            + "] param[" + juce::String (p) + "] snapshot="
                            + juce::String (snapshot, 4)
                            + " current=" + juce::String (current, 4));
                        break;
                    }
                }
            }
        }
    }

private:
    struct AISlotSnapshot {
        std::array<float, kMaxParamsPerPlugin> values {};
        int numParams = 0;
        bool valid = false;
    };
    std::array<AISlotSnapshot, kMaxPluginsInChain> aiSnapshot {};
};
