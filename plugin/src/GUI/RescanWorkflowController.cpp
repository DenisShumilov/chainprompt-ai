#include "RescanWorkflowController.h"
#include "../Core/ChainPromptAudioProcessor.h"
#include <thread>

RescanWorkflowController::RescanWorkflowController (ChainPromptAudioProcessor& proc)
    : processor (proc)
{
}

RescanWorkflowController::~RescanWorkflowController()
{
    if (rescanListenerId != 0)
        processor.getPluginCache().removeRescanListener (rescanListenerId);
}

void RescanWorkflowController::startRescan()
{
    auto& cache = processor.getPluginCache();

    // Remove previous listener if any
    if (rescanListenerId != 0)
    {
        cache.removeRescanListener (rescanListenerId);
        rescanListenerId = 0;
    }

    // Register for rescan diff (fired on Message Thread via AsyncUpdater)
    rescanListenerId = cache.addRescanListener (
        [this] (const GlobalPluginCache::RescanDiff& diff) {
            handleRescanComplete (diff);
        });

    cache.requestRescan();

    if (postToChat)
        postToChat ("System: Plugin rescan started...\n\n");
}

void RescanWorkflowController::handleRescanComplete (const GlobalPluginCache::RescanDiff& diff)
{
    juce::String msg = "System: Rescan complete (" + diff.scanMode + "). " +
        juce::String (diff.totalInstalled) + " plugins installed.\n";

    if (diff.added.size() > 0)
    {
        msg += "  + Added: ";
        for (int i = 0; i < diff.added.size(); ++i)
        {
            if (i > 0) msg += ", ";
            if (i >= 10) { msg += "... (+" + juce::String (diff.added.size() - 10) + " more)"; break; }
            msg += diff.added[i];
        }
        msg += "\n";
    }

    if (diff.removed.size() > 0)
    {
        msg += "  - Removed: ";
        for (int i = 0; i < diff.removed.size(); ++i)
        {
            if (i > 0) msg += ", ";
            if (i >= 10) { msg += "... (+" + juce::String (diff.removed.size() - 10) + " more)"; break; }
            msg += diff.removed[i];
        }
        msg += "\n";
    }

    if (diff.updated.size() > 0)
    {
        msg += "  ~ Updated: ";
        for (int i = 0; i < diff.updated.size(); ++i)
        {
            if (i > 0) msg += ", ";
            if (i >= 10) { msg += "... (+" + juce::String (diff.updated.size() - 10) + " more)"; break; }
            msg += diff.updated[i];
        }
        msg += "\n";
    }

    if (diff.added.size() == 0 && diff.removed.size() == 0 && diff.updated.size() == 0)
        msg += "  No changes detected.\n";

    msg += "\n";

    if (postToChat)
        postToChat (msg);

    if (onRefreshSidebar)
        onRefreshSidebar();

    // Notify Python server to hot-reload the installed manifest.
    // On failure, surface a visible warning to the operator.
    // Note: chatFn is captured by value (safe for async). healthChecker is
    // accessed only via MessageManager::callAsync, which runs on the message
    // thread where the processor is guaranteed alive.
    auto chatFn = postToChat;
    auto* proc = &processor;

    std::thread ([chatFn, proc]() {
        juce::URL rescanURL ("http://127.0.0.1:5000/api/rescan");
        rescanURL = rescanURL.withPOSTData ("{}");
        int statusCode = 0;
        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs (5000)
            .withExtraHeaders ("Content-Type: application/json")
            .withHttpRequestCmd ("POST")
            .withStatusCode (&statusCode);
        auto stream = rescanURL.createInputStream (opts);

        if (stream != nullptr && statusCode >= 200 && statusCode < 300)
        {
            DBG ("RescanWorkflow: notified Python server of rescan");
            // Trigger immediate health re-check on the message thread
            // where the processor is guaranteed alive.
            juce::MessageManager::callAsync ([proc]() {
                proc->healthChecker.checkNow();
            });
        }
        else
        {
            DBG ("RescanWorkflow: failed to notify Python server (status=" +
                 juce::String (statusCode) + ")");

            // Surface the failure to the operator on the message thread
            if (chatFn)
            {
                juce::MessageManager::callAsync ([chatFn]() {
                    chatFn ("System: WARNING — Local rescan succeeded but the Python backend "
                            "did not reload the catalog.\n"
                            "  The backend may not be running. Start it with: python server.py\n"
                            "  Then click Rescan again to sync.\n\n");
                });
            }
        }
    }).detach();
}
