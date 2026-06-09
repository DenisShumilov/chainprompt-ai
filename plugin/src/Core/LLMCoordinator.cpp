#include "LLMCoordinator.h"
#include <algorithm>

LLMCoordinator::LLMCoordinator()
    : juce::Thread ("LLM_Thread")
{
}

LLMCoordinator::~LLMCoordinator()
{
    stopThread (4000);
}

void LLMCoordinator::submitPrompt (const juce::String& userText)
{
    {
        const juce::ScopedLock sl (promptLock);
        pendingPrompt = userText;
        pendingHasAudio = false;
        pendingAudioBase64 = {};
        pendingAudioMimeType = {};
        pendingMetrics = {};
        hasNewPrompt = true;
    }

    notify();
}

void LLMCoordinator::submitPromptWithAudio (const juce::String& userText,
                                             float rmsDB, float peakDB, float crestFactor,
                                             float zcr, double bpm,
                                             float subPct, float lowPct, float lowMidPct,
                                             float highMidPct, float airPct,
                                             bool isStereo,
                                             const juce::String& audioBase64,
                                             const juce::String& audioMimeType,
                                             const std::array<float, 31>& spectrum31,
                                             float mdTserDb,
                                             float spectralFlux,
                                             float oeHerDb,
                                             bool isTonal,
                                             const std::array<float, 4>& bwIcc,
                                             float lrBalanceDb,
                                             const juce::String& audioPath)
{
    {
        const juce::ScopedLock sl (promptLock);
        pendingPrompt = userText;
        pendingMetrics = { rmsDB, peakDB, crestFactor, zcr, bpm,
                           subPct, lowPct, lowMidPct, highMidPct, airPct,
                           isStereo, spectrum31, mdTserDb, spectralFlux, oeHerDb, isTonal, bwIcc,
                           lrBalanceDb };
        pendingHasAudio = true;
        pendingAudioBase64 = audioBase64;
        pendingAudioMimeType = audioMimeType;
        pendingAudioPath = audioPath;
        hasNewPrompt = true;
    }

    notify();
}

void LLMCoordinator::run()
{
    // Load plugin descriptions on thread start
    loadDescriptions();

    while (! threadShouldExit())
    {
        wait (-1);

        if (threadShouldExit())
            break;

        juce::String userText;
        PendingMetrics metrics {};
        bool hasAudio = false;
        juce::String audioBase64;
        juce::String audioMimeType;
        juce::String audioPath;
        juce::Array<juce::var> historyArray;
        std::vector<ChainSlotInfo> currentChain;
        juce::String instId;
        juce::String trkName;
        juce::String projScope;
        bool isNewChat = false;

        {
            const juce::ScopedLock sl (promptLock);
            if (! hasNewPrompt)
                continue;
            userText = pendingPrompt;
            metrics = pendingMetrics;
            hasAudio = pendingHasAudio;
            audioBase64 = pendingAudioBase64;
            audioMimeType = pendingAudioMimeType;
            audioPath = pendingAudioPath;
            currentChain = pendingCurrentChain;
            instId = pendingInstanceId;
            trkName = pendingTrackName;
            projScope = pendingProjectScope;
            isNewChat = pendingNewChat;
            hasNewPrompt = false;
            pendingNewChat = false;

            // Push the current user message to history
            chatHistory.push_back ({ "user", userText, userText });

            // Build chat history for server (exclude the last entry —
            // it's the current prompt, which the server appends separately).
            // Sliding window: send at most the last kMaxHistoryMessages to
            // prevent token overflow on long conversations.
            constexpr int kMaxHistoryMessages = 12;
            int totalPrior = static_cast<int> (chatHistory.size()) - 1;
            int startIdx = std::max (0, totalPrior - kMaxHistoryMessages);

            for (int i = startIdx; i < totalPrior; ++i)
            {
                auto& entry = chatHistory[static_cast<size_t> (i)];
                auto* obj = new juce::DynamicObject();
                obj->setProperty ("role", entry.role);
                obj->setProperty ("text", entry.rawText);
                historyArray.add (juce::var (obj));
            }
        }

        DBG ("LLMCoordinator: sending to server: " + userText);
        isProcessing.store (true, std::memory_order_relaxed);

        // --- Build JSON request for local Python server ---
        auto* requestBody = new juce::DynamicObject();
        requestBody->setProperty ("prompt", userText);
        requestBody->setProperty ("history", historyArray);
        requestBody->setProperty ("instance_id", instId);
        requestBody->setProperty ("track_name", trkName);

        if (isNewChat)
            requestBody->setProperty ("is_new_chat", true);

        // Current chain: full slot contract (uid, slotId, intent flags)
        {
            juce::Array<juce::var> chainArray;
            for (const auto& slot : currentChain)
            {
                auto* entry = new juce::DynamicObject();
                entry->setProperty ("uid", slot.uid);
                entry->setProperty ("slot_id", slot.slotId);
                entry->setProperty ("user_touched", slot.userTouched);
                entry->setProperty ("bypassed", slot.bypassed);
                entry->setProperty ("user_owned", slot.userOwned);
                chainArray.add (juce::var (entry));
            }
            requestBody->setProperty ("current_chain", chainArray);
        }

        // Project scope: shared across all instances in the same DAW session
        if (projScope.isNotEmpty())
            requestBody->setProperty ("project_scope", projScope);

        // Only include metrics + audio when the user captured audio
        if (hasAudio)
        {
            auto* metricsObj = new juce::DynamicObject();
            metricsObj->setProperty ("rmsDB",        metrics.rmsDB);
            metricsObj->setProperty ("peakDB",       metrics.peakDB);
            metricsObj->setProperty ("crestFactor",  metrics.crestFactor);
            metricsObj->setProperty ("zcr",          metrics.zcr);
            metricsObj->setProperty ("bpm",          metrics.bpm);
            metricsObj->setProperty ("subPct",       metrics.subPct);
            metricsObj->setProperty ("lowPct",       metrics.lowPct);
            metricsObj->setProperty ("lowMidPct",    metrics.lowMidPct);
            metricsObj->setProperty ("highMidPct",   metrics.highMidPct);
            metricsObj->setProperty ("airPct",       metrics.airPct);
            metricsObj->setProperty ("isStereo",     metrics.isStereo);

            // 31-band 1/3-octave spectrum (dB values)
            {
                juce::Array<juce::var> specArray;
                bool hasSpectrum = false;
                for (int i = 0; i < 31; ++i)
                {
                    float v = metrics.spectrum31[static_cast<size_t> (i)];
                    specArray.add (juce::var (static_cast<double> (v)));
                    if (v < -0.5f || v > 0.5f) hasSpectrum = true;
                }
                if (hasSpectrum)
                {
                    metricsObj->setProperty ("spectrum31", specArray);
                    DBG ("LLMCoordinator: spectrum31 attached (" + juce::String (specArray.size()) + " bands)");
                }
            }

            metricsObj->setProperty ("mdTserDb",     static_cast<double> (metrics.mdTserDb));
            metricsObj->setProperty ("spectralFlux", static_cast<double> (metrics.spectralFlux));
            metricsObj->setProperty ("oeHerDb",      static_cast<double> (metrics.oeHerDb));
            metricsObj->setProperty ("isTonal",      metrics.isTonal);

            // BW-ICC: 4-band inter-channel coherence [sub, lowMid, hiMid, high]
            {
                juce::Array<juce::var> iccArray;
                for (int i = 0; i < 4; ++i)
                    iccArray.add (juce::var (static_cast<double> (metrics.bwIcc[static_cast<size_t> (i)])));
                metricsObj->setProperty ("bwIcc", iccArray);
            }

            // L/R Energy Balance (dB): +dB = left-heavy, -dB = right-heavy
            metricsObj->setProperty ("lrBalanceDb", static_cast<double> (metrics.lrBalanceDb));

            requestBody->setProperty ("metrics", juce::var (metricsObj));

            // Prefer file path over Base64 (avoids huge JSON payload on localhost)
            if (audioPath.isNotEmpty())
            {
                // Convert Windows backslashes to forward slashes for JSON/Python
                requestBody->setProperty ("audioPath", audioPath.replace ("\\", "/"));
                requestBody->setProperty ("audioMimeType", audioMimeType);
            }
            else if (audioBase64.isNotEmpty())
            {
                requestBody->setProperty ("audioBase64", audioBase64);
                requestBody->setProperty ("audioMimeType", audioMimeType);
            }
        }

        juce::String requestString = juce::JSON::toString (juce::var (requestBody));

        // --- HTTP POST to local Python server ---
        juce::URL apiURL ("http://127.0.0.1:5000/api/chat");
        apiURL = apiURL.withPOSTData (requestString);

        int statusCode = 0;
        auto streamOptions = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                 .withConnectionTimeoutMs (120000)
                                 .withExtraHeaders ("Content-Type: application/json")
                                 .withHttpRequestCmd ("POST")
                                 .withStatusCode (&statusCode);

        std::unique_ptr<juce::InputStream> stream (apiURL.createInputStream (streamOptions));

        if (stream == nullptr)
        {
            DBG ("LLMCoordinator: cannot connect to server (timeout or server down)");
            isProcessing.store (false, std::memory_order_relaxed);
            if (onChatMessage)
                juce::MessageManager::callAsync ([this]()
                {
                    if (onChatMessage)
                        onChatMessage ("AI", "[Server timeout or unreachable at 127.0.0.1:5000. Is server.py running?]");
                });
            continue;
        }

        if (statusCode < 200 || statusCode >= 300)
        {
            isProcessing.store (false, std::memory_order_relaxed);
            juce::String errorBody = stream->readEntireStreamAsString();
            DBG ("LLMCoordinator: server error " + juce::String (statusCode) + ": " + errorBody);

            // Try to extract error message from JSON response
            juce::String errorMsg = "[Server error " + juce::String (statusCode) + "]";
            auto errorJson = juce::JSON::parse (errorBody);
            if (errorJson.isObject())
            {
                auto reply = errorJson.getProperty ("reply", "").toString();
                if (reply.isNotEmpty())
                    errorMsg = reply;
            }

            if (onChatMessage)
            {
                juce::MessageManager::callAsync ([this, errorMsg]()
                {
                    if (onChatMessage)
                        onChatMessage ("AI", errorMsg);
                });
            }
            continue;
        }

        juce::String responseText = stream->readEntireStreamAsString();
        DBG ("LLMCoordinator: server response: " + responseText);

        if (threadShouldExit())
            break;

        // --- Parse server response ---
        auto responseJson = juce::JSON::parse (responseText);

        if (! responseJson.isObject())
        {
            DBG ("LLMCoordinator: invalid JSON from server");
            isProcessing.store (false, std::memory_order_relaxed);
            if (onChatMessage)
                juce::MessageManager::callAsync ([this]()
                {
                    if (onChatMessage)
                        onChatMessage ("AI", "[Invalid response from server.]");
                });
            continue;
        }

        juce::String textForUI = responseJson.getProperty ("reply", "").toString();

        // Push model response to chat history
        {
            const juce::ScopedLock sl (promptLock);
            chatHistory.push_back ({ "model", textForUI, textForUI });
        }

        // Fire chat message callback on message thread
        if (textForUI.isNotEmpty() && onChatMessage)
        {
            juce::MessageManager::callAsync ([this, textForUI]()
            {
                if (onChatMessage)
                    onChatMessage ("AI", textForUI);
            });
        }

        // --- Parse and apply preset if present (Step 2: chat returns no preset) ---
        auto presetVar = responseJson.getProperty ("preset", juce::var ());

        if (presetVar.isObject())
        {
            ChainPreset preset;
            preset.numPlugins = 0;

            auto pluginsArr = presetVar.getProperty ("plugins", juce::var());
            if (pluginsArr.isArray())
            {
                for (int i = 0; i < pluginsArr.getArray()->size() && i < kMaxPluginsInChain; ++i)
                {
                    auto pluginObj = pluginsArr.getArray()->getReference (i);
                    auto& pluginCfg = preset.plugins[static_cast<size_t> (preset.numPlugins)];

                    pluginCfg.uid = static_cast<int> (pluginObj.getProperty ("uid", 0));
                    pluginCfg.slotId = static_cast<int> (pluginObj.getProperty ("slot_id", 0));
                    parsePluginParams (pluginObj, pluginCfg);

                    // Parse slot action from server (authoritative contract)
                    auto actionStr = pluginObj.getProperty ("action", "keep").toString().toLowerCase();
                    uint8_t action = SlotActions::Keep;
                    if (actionStr == "insert")            action = SlotActions::Insert;
                    else if (actionStr == "bypass")       action = SlotActions::Bypass;
                    else if (actionStr == "enable")       action = SlotActions::Enable;
                    else if (actionStr == "reconfigure")  action = SlotActions::Reconfigure;
                    preset.slotAction[static_cast<size_t> (preset.numPlugins)] = action;

                    preset.numPlugins++;
                }
            }

            if (onPresetGenerated)
            {
                onPresetGenerated (preset);
                DBG ("LLMCoordinator: preset applied (" + juce::String (preset.numPlugins) + " plugins)");
            }
        }
        // else: chat-only response — no DSP mutation (Step 2)

        isProcessing.store (false, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// Helper: parse plugin params from JSON into PluginConfig
// ---------------------------------------------------------------------------
void LLMCoordinator::parsePluginParams (const juce::var& pluginObj, PluginConfig& pluginCfg)
{
    auto paramsArray = pluginObj.getProperty ("params", juce::var());

    if (! paramsArray.isArray())
    {
        pluginCfg.numParams = 0;
        return;
    }

    pluginCfg.numParams = 0;

    for (int j = 0; j < paramsArray.getArray()->size() && j < kMaxParamsPerPlugin; ++j)
    {
        auto paramObj = paramsArray.getArray()->getReference (j);
        int idx = static_cast<int> (paramObj.getProperty ("index", -1));

        // IRON DOME: skip bypass parameter
        if (idx == 0)
            continue;

        // Server already translates physical → normalized via iron_dome_filter().
        // Do NOT re-translate here — value is already a 0.0-1.0 VST float.
        float normalized = static_cast<float> (paramObj.getProperty ("value", 0.0));
        normalized = juce::jlimit (0.0f, 1.0f, normalized);

        pluginCfg.params[static_cast<size_t> (pluginCfg.numParams)].index = idx;
        pluginCfg.params[static_cast<size_t> (pluginCfg.numParams)].value = normalized;
        pluginCfg.numParams++;
    }
}

// ---------------------------------------------------------------------------
// Helper: strip Mono/Stereo/Live suffixes to get base plugin name
// ---------------------------------------------------------------------------
juce::String LLMCoordinator::getBaseName (const juce::String& fullName) const
{
    juce::String base = fullName;

    // Strip Mono/Stereo variants first
    if (base.endsWith (" Mono/Stereo"))
        base = base.dropLastCharacters (12);
    else if (base.endsWith (" Stereo"))
        base = base.dropLastCharacters (7);
    else if (base.endsWith (" Mono"))
        base = base.dropLastCharacters (5);

    // Handle surround suffixes like "5.0/Stereo", "5.1/Stereo", "Quad/Stereo"
    if (base.endsWith ("/Stereo"))
    {
        int lastSpace = base.lastIndexOfChar (' ');
        if (lastSpace >= 0)
            base = base.substring (0, lastSpace);
    }

    // Handle Quad suffix
    if (base.endsWith (" Quad"))
        base = base.dropLastCharacters (5);

    // Strip Live suffix AFTER Mono/Stereo (handles "X Live Stereo" -> "X Live" -> "X")
    if (base.endsWith (" Live"))
        base = base.dropLastCharacters (5);

    return base.trim();
}

// ---------------------------------------------------------------------------
// Load plugin_descriptions.json
// ---------------------------------------------------------------------------
void LLMCoordinator::loadDescriptions()
{
    // Canonical location: repo data/metadata/ (set at build time via CHAINPROMPT_DATA_DIR)
    // Fallback: %APPDATA%/ChainPrompt/ for deployed/packaged scenarios
#ifdef CHAINPROMPT_DATA_DIR
    juce::File descFile = juce::File (CHAINPROMPT_DATA_DIR)
                              .getChildFile ("metadata")
                              .getChildFile ("plugin_descriptions.json");
#else
    juce::File descFile = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                              .getChildFile ("ChainPrompt")
                              .getChildFile ("plugin_descriptions.json");
#endif

    if (! descFile.existsAsFile())
    {
        DBG ("LLMCoordinator: plugin_descriptions.json not found at " + descFile.getFullPathName());
        DBG ("LLMCoordinator: Expected canonical location is <repo>/data/metadata/plugin_descriptions.json");
        return;
    }

    auto descVar = juce::JSON::parse (descFile);
    if (! descVar.isObject())
    {
        DBG ("LLMCoordinator: Failed to parse plugin_descriptions.json");
        return;
    }

    auto* descObj = descVar.getDynamicObject();
    if (descObj != nullptr)
    {
        for (auto& prop : descObj->getProperties())
            pluginDescriptions[prop.name.toString()] = prop.value.toString();
    }

    DBG ("LLMCoordinator: Loaded " + juce::String ((int) pluginDescriptions.size()) + " plugin descriptions");
}


