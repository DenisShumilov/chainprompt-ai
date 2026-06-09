#pragma once

#include <JuceHeader.h>
#include "../Core/GlobalPluginCache.h"

class ChainPromptAudioProcessor;

/**
 * RescanWorkflowController — Encapsulates the full rescan lifecycle:
 *   start scan → format diff message → notify Python → refresh sidebar.
 *
 * Uses removable rescan listener on GlobalPluginCache (safe destruction).
 */
class RescanWorkflowController
{
public:
    explicit RescanWorkflowController (ChainPromptAudioProcessor& proc);
    ~RescanWorkflowController();

    // Callbacks (set by editor after construction)
    std::function<void (const juce::String&)> postToChat;
    std::function<void()> onRefreshSidebar;

    /** Kick off a full plugin rescan. */
    void startRescan();

private:
    void handleRescanComplete (const GlobalPluginCache::RescanDiff& diff);

    ChainPromptAudioProcessor& processor;
    int rescanListenerId = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RescanWorkflowController)
};
