#pragma once

/**
 * ChainStateSerializer — DAW state save/load + undo capture helpers.
 *
 * Extracted from ChainPromptAudioProcessor (Sprint 11: Great Decoupling, Step 1).
 * Stateless namespace of free functions — no circular dependencies.
 *
 * Usage:
 *   ChainStateSerializer::serialize (destData, ...);
 *   auto state = ChainStateSerializer::deserialize (data, size);
 *   ChainStateSerializer::capturePluginStates (chainNodes, preset);
 */

#include <JuceHeader.h>
#include "PresetData.h"
#include "LLMCoordinator.h"

namespace ChainStateSerializer
{

// ── Capture live plugin states (Base64) from graph nodes into preset ──
// Call on message thread only. Used by undo/redo and getStateInformation.
inline void capturePluginStates (
    const std::vector<juce::AudioProcessorGraph::Node::Ptr>& chainNodes,
    ChainPreset& preset)
{
    for (size_t i = 0; i < chainNodes.size(); ++i)
    {
        if (chainNodes[i] != nullptr)
        {
            if (auto* proc = dynamic_cast<juce::AudioPluginInstance*> (chainNodes[i]->getProcessor()))
            {
                juce::MemoryBlock block;
                proc->getStateInformation (block);
                preset.pluginStateBase64[i] = block.toBase64Encoding();
            }
        }
    }
}

// ── Serialize full processor state to MemoryBlock (DAW save) ──
inline void serialize (
    juce::MemoryBlock& destData,
    const juce::String& instanceId,
    const juce::String& trackName,
    int nextSlotId,
    const std::vector<juce::AudioProcessorGraph::Node::Ptr>& chainNodes,
    const ChainPreset& currentPreset,
    bool hasPendingPreset,
    const ChainPreset& pendingPreset,
    LLMCoordinator& llmCoordinator)
{
    auto rootObj = new juce::DynamicObject();

    // Studio Memory identity
    rootObj->setProperty ("instanceId", instanceId);
    rootObj->setProperty ("trackName", trackName);
    rootObj->setProperty ("nextSlotId", nextSlotId);

    // Chat history
    juce::Array<juce::var> chatArray;
    for (const auto& entry : llmCoordinator.getChatHistory())
    {
        auto msgObj = new juce::DynamicObject();
        msgObj->setProperty ("role", entry.role);
        msgObj->setProperty ("rawText", entry.rawText);
        msgObj->setProperty ("uiText", entry.uiText);
        chatArray.add (juce::var (msgObj));
    }
    rootObj->setProperty ("chat", chatArray);

    // Plugin chain
    juce::Array<juce::var> pluginsArray;

    if (! chainNodes.empty())
    {
        for (size_t i = 0; i < chainNodes.size(); ++i)
        {
            auto pluginObj = new juce::DynamicObject();

            if (auto* proc = chainNodes[i] != nullptr
                    ? dynamic_cast<juce::AudioPluginInstance*> (chainNodes[i]->getProcessor())
                    : nullptr)
            {
                pluginObj->setProperty ("uid", proc->getPluginDescription().uniqueId);
                pluginObj->setProperty ("slotId", currentPreset.plugins[i].slotId);

                juce::MemoryBlock block;
                proc->getStateInformation (block);
                pluginObj->setProperty ("stateBase64", block.toBase64Encoding());
            }

            pluginsArray.add (juce::var (pluginObj));
        }
    }
    else if (hasPendingPreset)
    {
        for (int i = 0; i < pendingPreset.numPlugins; ++i)
        {
            auto si = static_cast<size_t> (i);
            auto pluginObj = new juce::DynamicObject();
            pluginObj->setProperty ("uid", pendingPreset.plugins[si].uid);
            pluginObj->setProperty ("slotId", pendingPreset.plugins[si].slotId);

            if (pendingPreset.pluginStateBase64[si].isNotEmpty())
                pluginObj->setProperty ("stateBase64", pendingPreset.pluginStateBase64[si]);

            pluginsArray.add (juce::var (pluginObj));
        }
    }

    rootObj->setProperty ("preset", pluginsArray);

    // Slot flags: use correct count for pending-preset edge case
    const int flagCount = hasPendingPreset
                              ? pendingPreset.numPlugins
                              : currentPreset.numPlugins;

    juce::Array<juce::var> bypassArray;
    for (int i = 0; i < flagCount; ++i)
        bypassArray.add (currentPreset.bypassed[static_cast<size_t> (i)]);
    rootObj->setProperty ("bypass", bypassArray);

    juce::Array<juce::var> userTouchedArray;
    for (int i = 0; i < flagCount; ++i)
        userTouchedArray.add (currentPreset.userTouched[static_cast<size_t> (i)]);
    rootObj->setProperty ("userTouched", userTouchedArray);

    juce::Array<juce::var> userOwnedArray;
    for (int i = 0; i < flagCount; ++i)
        userOwnedArray.add (currentPreset.userOwned[static_cast<size_t> (i)]);
    rootObj->setProperty ("userOwned", userOwnedArray);

    juce::String jsonString = juce::JSON::toString (juce::var (rootObj));
    destData.append (jsonString.toRawUTF8(), jsonString.getNumBytesAsUTF8());
}

// ── Deserialized state container ──
struct DeserializedState
{
    juce::String instanceId;
    juce::String trackName;
    int nextSlotId = 0;
    std::vector<ChatMessage> chatHistory;
    ChainPreset preset;
    bool valid = false;
};

// ── Deserialize DAW-saved state from raw bytes ──
inline DeserializedState deserialize (const void* data, int sizeInBytes)
{
    DeserializedState result;

    juce::String jsonString = juce::String::fromUTF8 (
        static_cast<const char*> (data), sizeInBytes);

    auto parsed = juce::JSON::parse (jsonString);
    if (parsed.isVoid())
        return result;

    auto* rootObj = parsed.getDynamicObject();
    if (rootObj == nullptr)
        return result;

    // Instance identity
    result.instanceId = rootObj->getProperty ("instanceId").toString();
    result.trackName  = rootObj->getProperty ("trackName").toString();
    result.nextSlotId = rootObj->hasProperty ("nextSlotId")
                            ? static_cast<int> (rootObj->getProperty ("nextSlotId"))
                            : 0;

    // Chat history
    if (auto* chatArr = rootObj->getProperty ("chat").getArray())
    {
        for (const auto& item : *chatArr)
        {
            if (auto* msgObj = item.getDynamicObject())
            {
                juce::String role    = msgObj->getProperty ("role").toString();
                juce::String rawText = msgObj->getProperty ("rawText").toString();
                juce::String uiText  = msgObj->getProperty ("uiText").toString();

                if (rawText.isEmpty())
                    rawText = msgObj->getProperty ("text").toString();

                result.chatHistory.push_back ({ role, rawText, uiText });
            }
        }
    }

    // Preset chain
    auto& preset = result.preset;
    if (auto* pluginsArray = rootObj->getProperty ("preset").getArray())
    {
        preset.numPlugins = juce::jmin (static_cast<int> (pluginsArray->size()), kMaxPluginsInChain);
        for (int i = 0; i < preset.numPlugins; ++i)
        {
            auto si = static_cast<size_t> (i);
            if (auto* pluginObj = (*pluginsArray)[i].getDynamicObject())
            {
                preset.plugins[si].uid =
                    static_cast<int> (pluginObj->getProperty ("uid"));
                preset.plugins[si].slotId =
                    pluginObj->hasProperty ("slotId")
                        ? static_cast<int> (pluginObj->getProperty ("slotId"))
                        : 0;

                preset.pluginStateBase64[si] =
                    pluginObj->getProperty ("stateBase64").toString();

                if (auto* paramsArray = pluginObj->getProperty ("params").getArray())
                {
                    preset.plugins[si].numParams =
                        juce::jmin (static_cast<int> (paramsArray->size()), kMaxParamsPerPlugin);
                    for (int p = 0; p < preset.plugins[si].numParams; ++p)
                    {
                        auto sp = static_cast<size_t> (p);
                        if (auto* paramObj = (*paramsArray)[p].getDynamicObject())
                        {
                            preset.plugins[si].params[sp].index =
                                static_cast<int> (paramObj->getProperty ("index"));
                            preset.plugins[si].params[sp].value =
                                static_cast<float> (static_cast<double> (paramObj->getProperty ("value")));
                        }
                    }
                }
            }
        }
    }

    // Flags
    if (auto* bypassArr = rootObj->getProperty ("bypass").getArray())
    {
        for (int i = 0; i < juce::jmin (static_cast<int> (bypassArr->size()), kMaxPluginsInChain); ++i)
            preset.bypassed[static_cast<size_t> (i)] = static_cast<bool> ((*bypassArr)[i]);
    }

    if (auto* utArr = rootObj->getProperty ("userTouched").getArray())
    {
        for (int i = 0; i < juce::jmin (static_cast<int> (utArr->size()), kMaxPluginsInChain); ++i)
            preset.userTouched[static_cast<size_t> (i)] = static_cast<bool> ((*utArr)[i]);
    }

    if (auto* uoArr = rootObj->getProperty ("userOwned").getArray())
    {
        for (int i = 0; i < juce::jmin (static_cast<int> (uoArr->size()), kMaxPluginsInChain); ++i)
            preset.userOwned[static_cast<size_t> (i)] = static_cast<bool> ((*uoArr)[i]);
    }

    result.valid = true;
    return result;
}

} // namespace ChainStateSerializer
