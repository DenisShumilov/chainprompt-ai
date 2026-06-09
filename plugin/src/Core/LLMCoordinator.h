#pragma once

#include <JuceHeader.h>
#include "PresetData.h"

// ── Slot Action Contract (transport-only, not persisted) ──
// Server sends per-slot actions in the preset response JSON.
// C++ executes them and stores the resulting state in ChainPreset.
namespace SlotActions
{
    static constexpr uint8_t Keep        = 0;  // Retain plugin + params as-is
    static constexpr uint8_t Insert      = 1;  // New plugin (Stage 2 provides params)
    static constexpr uint8_t Bypass      = 2;  // Disconnect from signal chain (keep node alive)
    static constexpr uint8_t Enable      = 3;  // Reconnect a previously bypassed slot
    static constexpr uint8_t Reconfigure = 4;  // Retained plugin but Stage 2 provides new params
}

struct ChatMessage
{
    juce::String role;
    juce::String rawText;
    juce::String uiText;
};

class LLMCoordinator : public juce::Thread
{
public:
    LLMCoordinator();
    ~LLMCoordinator() override;

    /** Submit a chat-only prompt (no audio analysis). */
    void submitPrompt (const juce::String& userText);

    /** Submit a prompt WITH audio capture (metrics + audio file path or Base64). */
    void submitPromptWithAudio (const juce::String& userText,
                                float rmsDB, float peakDB, float crestFactor,
                                float zcr, double bpm,
                                float subPct, float lowPct, float lowMidPct,
                                float highMidPct, float airPct,
                                bool isStereo,
                                const juce::String& audioBase64,
                                const juce::String& audioMimeType,
                                const std::array<float, 31>& spectrum31 = {},
                                float mdTserDb = 0.0f,
                                float spectralFlux = 0.0f,
                                float oeHerDb = 0.0f,
                                bool isTonal = false,
                                const std::array<float, 4>& bwIcc = {{ 1.0f, 1.0f, 1.0f, 1.0f }},
                                float lrBalanceDb = 0.0f,
                                const juce::String& audioPath = {});

    std::function<void (const ChainPreset&)> onPresetGenerated;
    std::function<void (const juce::String& role, const juce::String& message)> onChatMessage;

    /** True while the LLM HTTP request is in flight. Read from UI timer. */
    std::atomic<bool> isProcessing { false };

    std::vector<ChatMessage> getChatHistory()
    {
        const juce::ScopedLock sl (promptLock);
        return chatHistory;
    }

    void clearChatHistory()
    {
        const juce::ScopedLock sl (promptLock);
        chatHistory.clear();
        pendingNewChat = true;
    }

    /** Set UIDs + slotIds + intent flags of currently loaded plugins (call from message thread before submit). */
    struct ChainSlotInfo {
        int uid = 0;
        int slotId = 0;
        bool userTouched = false;   // user modified parameters since last AI action
        bool bypassed = false;      // user explicitly bypassed this slot
        bool userOwned = false;     // user manually added this plugin (keep it, but AI may configure)
    };

    void setCurrentChain (const std::vector<ChainSlotInfo>& slotInfos)
    {
        const juce::ScopedLock sl (promptLock);
        pendingCurrentChain = slotInfos;
    }

    /** Set instance identity for Studio Memory (call from message thread). */
    void setInstanceInfo (const juce::String& instId, const juce::String& trkName,
                          const juce::String& projScope = {})
    {
        const juce::ScopedLock sl (promptLock);
        pendingInstanceId = instId;
        pendingTrackName = trkName;
        pendingProjectScope = projScope;
    }

    void restoreChatHistory (std::vector<ChatMessage> restored)
    {
        const juce::ScopedLock sl (promptLock);
        chatHistory = std::move (restored);
    }

private:
    void run() override;

    struct PendingMetrics
    {
        float rmsDB = -90.0f;
        float peakDB = -90.0f;
        float crestFactor = 0.0f;
        float zcr = 0.0f;
        double bpm = 120.0;
        float subPct = 0.0f;
        float lowPct = 0.0f;
        float lowMidPct = 0.0f;
        float highMidPct = 0.0f;
        float airPct = 0.0f;
        bool isStereo = true;
        std::array<float, 31> spectrum31 {};
        float mdTserDb = 0.0f;
        float spectralFlux = 0.0f;
        float oeHerDb = 0.0f;
        bool isTonal = false;
        std::array<float, 4> bwIcc {{ 1.0f, 1.0f, 1.0f, 1.0f }};
        float lrBalanceDb = 0.0f;
    };

    juce::CriticalSection promptLock;
    juce::String pendingPrompt;
    PendingMetrics pendingMetrics;
    bool hasNewPrompt = false;
    bool pendingHasAudio = false;
    juce::String pendingAudioBase64;
    juce::String pendingAudioMimeType;
    juce::String pendingAudioPath;  // temp file path (preferred over Base64)
    bool pendingNewChat = false;    // true after clearChatHistory() — tells server to forget history

    std::vector<ChatMessage> chatHistory;
    std::vector<ChainSlotInfo> pendingCurrentChain;
    juce::String pendingInstanceId;
    juce::String pendingTrackName;
    juce::String pendingProjectScope;

    // Plugin descriptions (base name -> description)
    std::map<juce::String, juce::String> pluginDescriptions;
    void loadDescriptions();
    juce::String getBaseName (const juce::String& fullName) const;

    /** Parse params array from a plugin JSON object into PluginConfig. */
    static void parsePluginParams (const juce::var& pluginObj, PluginConfig& pluginCfg);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LLMCoordinator)
};
