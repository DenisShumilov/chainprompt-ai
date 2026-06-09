#pragma once

#include <JuceHeader.h>
#include <atomic>

// ── Dynamic Automation Slot ──
// Pre-allocated parameter exposed to the DAW host for automation.
// Name is mutable — updated at runtime when the chain topology changes.
// Target pointer is atomic: written on message thread, read on any thread.

class DynamicSlotParameter : public juce::AudioParameterFloat
{
public:
    explicit DynamicSlotParameter (int slotIndex)
        : juce::AudioParameterFloat (
              juce::ParameterID ("slot_" + juce::String (slotIndex).paddedLeft ('0', 4), 1),
              "---",
              juce::NormalisableRange<float> (0.0f, 1.0f),
              0.0f)
    {}

    juce::String getName (int maxLen) const override
    {
        return displayName.substring (0, maxLen);
    }

    void setDisplayName (const juce::String& name) { displayName = name; }

    // Atomic target: the plugin parameter this slot forwards automation to.
    // nullptr = unmapped (slot is inert).
    std::atomic<juce::AudioProcessorParameter*> targetParam { nullptr };

    // ── Stable Identity Metadata (Step 5: automation identity foundation) ──
    // Tracks which logical slot + parameter this pool entry maps to.
    // slotId is monotonic and survives reorder, so this identity persists
    // across topology changes. Used for diagnostics and future native DAW automation.
    int mappedSlotId = 0;
    int mappedParamIndex = 0;

    void setStableMapping (int slotId, int paramIdx)
    {
        mappedSlotId = slotId;
        mappedParamIndex = paramIdx;
    }

private:
    juce::String displayName { "---" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicSlotParameter)
};
