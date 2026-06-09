#pragma once

/**
 * AutomationPoolRouter — Stable automation pool remapping engine.
 *
 * Extracted from ChainPromptAudioProcessor (Sprint 11: Great Decoupling, Step 4).
 * Owns the AutomationKey registry and manages {slotId, paramIndex} → pool slot
 * assignments for automation curve survival across topology changes.
 *
 * Usage:
 *   router.remap (chainNodes, currentPreset, paramSlots);
 *   int mapped = router.getActiveMappedSlots();
 */

#include <JuceHeader.h>
#include "PresetData.h"
#include "DynamicSlotParameter.h"
#include <unordered_map>
#include <vector>

class AutomationPoolRouter
{
public:
    /** Remap all pool slots to match current chain topology.
        Preserves existing pool positions where possible (automation stability).
        Returns number of mapped slots. Caller should call updateHostDisplay() after. */
    int remap (
        const std::vector<juce::AudioProcessorGraph::Node::Ptr>& chainNodes,
        const ChainPreset& currentPreset,
        std::array<DynamicSlotParameter*, kAutomationPoolSize>& paramSlots)
    {
        // ── Phase 1: Snapshot old registry for stable re-mapping ──
        auto oldRegistry = registry;

        // ── Phase 2: Collect new mappings ──
        struct PendingMapping {
            int slotId;
            int paramIndex;
            juce::AudioProcessorParameter* param;
            juce::String displayName;
        };
        std::vector<PendingMapping> pendingMappings;
        pendingMappings.reserve (512);

        for (size_t ni = 0; ni < chainNodes.size(); ++ni)
        {
            if (chainNodes[ni] == nullptr) continue;
            auto* proc = chainNodes[ni]->getProcessor();
            if (proc == nullptr) continue;

            auto& params = proc->getParameters();
            juce::String pluginName = proc->getName();
            int slotId = (ni < static_cast<size_t> (currentPreset.numPlugins))
                             ? currentPreset.plugins[ni].slotId : 0;

            // Skip param index 0 (bypass) — start from 1
            for (int pi = 1; pi < params.size(); ++pi)
            {
                auto* param = params[pi];
                if (param == nullptr) continue;

                pendingMappings.push_back ({
                    slotId, pi, param,
                    "[S" + juce::String (slotId) + "] " + pluginName + ": " + param->getName (50)
                });
            }
        }

        // ── Phase 3: Clear all pool slots ──
        for (int i = 0; i < kAutomationPoolSize; ++i)
        {
            paramSlots[static_cast<size_t> (i)]->targetParam.store (nullptr, std::memory_order_release);
            paramSlots[static_cast<size_t> (i)]->setDisplayName ("---");
            paramSlots[static_cast<size_t> (i)]->setStableMapping (0, 0);
        }

        // ── Phase 4: Assign pool positions — prefer old position for stability ──
        std::vector<bool> poolUsed (kAutomationPoolSize, false);
        std::vector<int> assignedPool (pendingMappings.size(), -1);
        registry.clear();

        // Pass 1: Re-claim old positions for params that existed before
        for (size_t m = 0; m < pendingMappings.size(); ++m)
        {
            AutomationKey key { pendingMappings[m].slotId, pendingMappings[m].paramIndex };
            auto it = oldRegistry.find (key);
            if (it != oldRegistry.end() && it->second >= 0 && it->second < kAutomationPoolSize
                && ! poolUsed[static_cast<size_t> (it->second)])
            {
                assignedPool[m] = it->second;
                poolUsed[static_cast<size_t> (it->second)] = true;
            }
        }

        // Pass 2: Fill remaining with first-available sequential slots
        int nextFree = 0;
        for (size_t m = 0; m < pendingMappings.size(); ++m)
        {
            if (assignedPool[m] >= 0) continue;

            while (nextFree < kAutomationPoolSize && poolUsed[static_cast<size_t> (nextFree)])
                ++nextFree;
            if (nextFree >= kAutomationPoolSize) break;

            assignedPool[m] = nextFree;
            poolUsed[static_cast<size_t> (nextFree)] = true;
            ++nextFree;
        }

        // ── Phase 5: Apply assignments + rebuild registry ──
        int mappedCount = 0;
        for (size_t m = 0; m < pendingMappings.size(); ++m)
        {
            int poolIdx = assignedPool[m];
            if (poolIdx < 0) continue;

            auto& pm = pendingMappings[m];
            auto* slot = paramSlots[static_cast<size_t> (poolIdx)];

            slot->targetParam.store (pm.param, std::memory_order_release);
            slot->setDisplayName (pm.displayName);
            slot->setStableMapping (pm.slotId, pm.paramIndex);
            slot->setValueNotifyingHost (pm.param->getValue());

            registry[{ pm.slotId, pm.paramIndex }] = poolIdx;
            ++mappedCount;
        }

        activeMappedSlots = mappedCount;

        // Stability metric
        int stableCount = 0;
        for (size_t m = 0; m < pendingMappings.size(); ++m)
        {
            AutomationKey key { pendingMappings[m].slotId, pendingMappings[m].paramIndex };
            auto it = oldRegistry.find (key);
            if (it != oldRegistry.end() && assignedPool[m] == it->second)
                ++stableCount;
        }

        juce::Logger::writeToLog ("AutomationPool: mapped " + juce::String (mappedCount)
            + "/" + juce::String (kAutomationPoolSize) + " slots"
            + " (stable=" + juce::String (stableCount) + ")");

        return mappedCount;
    }

    int getActiveMappedSlots() const noexcept { return activeMappedSlots; }

private:
    struct AutomationKey {
        int slotId = 0;
        int paramIndex = 0;
        bool operator== (const AutomationKey& o) const noexcept
            { return slotId == o.slotId && paramIndex == o.paramIndex; }
    };
    struct AutomationKeyHash {
        size_t operator() (const AutomationKey& k) const noexcept
            { return static_cast<size_t> (k.slotId * 131071) ^ static_cast<size_t> (k.paramIndex); }
    };

    std::unordered_map<AutomationKey, int, AutomationKeyHash> registry;
    int activeMappedSlots = 0;
};
