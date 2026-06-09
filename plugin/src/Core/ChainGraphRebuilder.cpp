#include "ChainGraphRebuilder.h"
#include "ChainPromptAudioProcessor.h"
#include "../GUI/ChainPromptAudioProcessorEditor.h"

ChainGraphRebuilder::ChainGraphRebuilder (ChainPromptAudioProcessor& p)
    : processor (p)
{
    rebuildTimer.owner = this;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: apply LLM-generated parameters to a single graph node
// ─────────────────────────────────────────────────────────────────────────────
void ChainGraphRebuilder::applyParamsToNode (
    juce::AudioProcessorGraph::Node::Ptr node,
    const PluginConfig& pc,
    const juce::String& label)
{
    if (node == nullptr) return;
    auto* proc = node->getProcessor();
    if (proc == nullptr) return;

    auto& params = proc->getParameters();

    juce::String paramLog = "LLM params for " + label + " uid=" + juce::String (pc.uid)
        + " numParams=" + juce::String (pc.numParams) + ": ";

    for (int p = 0; p < pc.numParams; ++p)
    {
        const auto& paramSetting = pc.params[static_cast<size_t> (p)];
        if (paramSetting.index <= 0) continue;
        if (paramSetting.index >= params.size()) continue;

        if (auto* param = params[paramSetting.index])
            param->setValue (juce::jlimit (0.0f, 1.0f, paramSetting.value));

        if (p < 10)
            paramLog += "[" + juce::String (paramSetting.index) + "]=" + juce::String (paramSetting.value, 3) + " ";
    }
    if (pc.numParams > 10) paramLog += "...(" + juce::String (pc.numParams) + " total)";
    juce::Logger::writeToLog (paramLog);
}

// ─────────────────────────────────────────────────────────────────────────────
// connectNodes: wire source→dest with stereo/mono handling
// ─────────────────────────────────────────────────────────────────────────────
void ChainGraphRebuilder::connectNodes (
    juce::AudioProcessorGraph::Node::Ptr source,
    juce::AudioProcessorGraph::Node::Ptr dest)
{
    if (source == nullptr || dest == nullptr)
    {
        juce::Logger::writeToLog ("CRITICAL ROUTING FAILURE: null node passed to connectNodes!");
        return;
    }

    const int srcOut = source->getProcessor()->getTotalNumOutputChannels();
    const int dstIn  = dest->getProcessor()->getTotalNumInputChannels();

    juce::Logger::writeToLog ("connectNodes: \"" + source->getProcessor()->getName() + "\" (out=" + juce::String (srcOut)
        + ") -> \"" + dest->getProcessor()->getName() + "\" (in=" + juce::String (dstIn) + ")");

    if (srcOut == 0 || dstIn == 0)
    {
        juce::Logger::writeToLog ("CRITICAL ROUTING FAILURE: 0-channel node! src-out="
            + juce::String (srcOut) + " dst-in=" + juce::String (dstIn)
            + "  src=\"" + source->getProcessor()->getName()
            + "\"  dst=\"" + dest->getProcessor()->getName() + "\"");
        return;
    }

    using UK = juce::AudioProcessorGraph::UpdateKind;

    bool ok0 = processor.mainGraph->addConnection ({{source->nodeID, 0}, {dest->nodeID, 0}}, UK::none);
    if (!ok0)
        juce::Logger::writeToLog ("CRITICAL ROUTING FAILURE: " + source->getProcessor()->getName()
            + " ch0 -> " + dest->getProcessor()->getName() + " ch0  (addConnection returned false)");

    if (srcOut >= 2 && dstIn >= 2)
    {
        bool ok1 = processor.mainGraph->addConnection ({{source->nodeID, 1}, {dest->nodeID, 1}}, UK::none);
        if (!ok1)
            juce::Logger::writeToLog ("CRITICAL ROUTING FAILURE: " + source->getProcessor()->getName()
                + " ch1 -> " + dest->getProcessor()->getName() + " ch1");
    }
    else if (srcOut >= 2 && dstIn < 2)
    {
        juce::Logger::writeToLog ("ROUTE: Stereo->Mono for " + dest->getProcessor()->getName());
    }
    else if (srcOut < 2 && dstIn >= 2)
    {
        // Mono→Stereo fan-out: duplicate ch0 to ch1
        bool ok1 = processor.mainGraph->addConnection ({{source->nodeID, 0}, {dest->nodeID, 1}}, UK::none);
        if (!ok1)
            juce::Logger::writeToLog ("CRITICAL ROUTING FAILURE: " + source->getProcessor()->getName()
                + " ch0 -> " + dest->getProcessor()->getName() + " ch1 (mono-fan)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// applyPresetTopology: initiate crossfade → rebuild
// ─────────────────────────────────────────────────────────────────────────────
void ChainGraphRebuilder::applyPresetTopology (const ChainPreset& preset, bool isFromDAWLoad)
{
    rebuildPreset = preset;
    rebuildIsFromDAWLoad = isFromDAWLoad;

    // Phase 1: Start fade-out, schedule rebuild after ramp completes
    processor.crossfadeTarget.store (0.0f, std::memory_order_relaxed);

    rebuildTimer.owner = this;
    rebuildTimer.startTimer (50);  // ~50ms for audio thread to ramp to silence
}

// ─────────────────────────────────────────────────────────────────────────────
// performRebuild: Phase 2 — actual graph rebuild (after fade-out)
// ─────────────────────────────────────────────────────────────────────────────
void ChainGraphRebuilder::performRebuild()
{
    using UK = juce::AudioProcessorGraph::UpdateKind;

    const auto& preset = rebuildPreset;
    const bool isFromDAWLoad = rebuildIsFromDAWLoad;

    // ── Fail-Fast UID Validation ──
    {
        juce::StringArray missingNames;
        for (int i = 0; i < preset.numPlugins; ++i)
        {
            int uid = preset.plugins[static_cast<size_t> (i)].uid;
            if (processor.pluginCache->findPluginByUID (uid) == nullptr)
            {
                bool canRecycle = false;
                for (size_t j = 0; j < processor.chainNodes.size(); ++j)
                {
                    if (processor.chainNodes[j] != nullptr
                        && j < static_cast<size_t> (processor.currentPreset.numPlugins)
                        && processor.currentPreset.plugins[j].uid == uid)
                    {
                        canRecycle = true;
                        break;
                    }
                }
                if (!canRecycle)
                    missingNames.add ("UID " + juce::String (uid));
            }
        }

        if (missingNames.size() > 0)
        {
            juce::Logger::writeToLog ("performRebuild: REJECTED — unavailable plugins: "
                + missingNames.joinIntoString (", "));

            juce::MessageManager::callAsync ([weakProc = juce::Component::SafePointer<juce::Component>(nullptr),
                                              &proc = processor, missingNames]() {
                if (proc.llmCoordinator.onChatMessage)
                {
                    juce::String msg = "System: Chain rejected — plugins not installed: "
                        + missingNames.joinIntoString (", ") + "\n\n";
                    proc.llmCoordinator.onChatMessage ("System", msg);
                }
            });

            // Fade back in since we're not rebuilding
            processor.crossfadeTarget.store (1.0f, std::memory_order_relaxed);
            return;
        }
    }

    // Clear automation pool BEFORE removing nodes (prevents dangling pointers)
    for (int i = 0; i < kAutomationPoolSize; ++i)
        processor.paramSlots[static_cast<size_t> (i)]->targetParam.store (nullptr, std::memory_order_release);

    // Now output is near-silent — suspendProcessing won't cause audible glitch
    processor.suspendProcessing (true);
    processor.isRebuildingGraph.store (true, std::memory_order_release);

    if (auto* ed = dynamic_cast<ChainPromptAudioProcessorEditor*> (processor.getActiveEditor()))
        ed->clearHostedEditor();

    const double sr = processor.getSampleRate() > 0 ? processor.getSampleRate() : 44100.0;
    const int blockSz = processor.getBlockSize() > 0 ? processor.getBlockSize() : 512;

    // ── Smart Rebuild: diff old chain vs new preset ──
    // Step 1: Disconnect all edges (keep nodes alive)
    {
        auto connections = processor.mainGraph->getConnections();
        for (auto& conn : connections)
            processor.mainGraph->removeConnection (conn, UK::none);
    }

    // If IO nodes don't exist yet (first run), create them
    if (processor.audioInputNode == nullptr || processor.audioOutputNode == nullptr)
    {
        processor.mainGraph->clear (UK::none);
        processor.mainGraph->enableAllBuses();
        {
            juce::AudioProcessor::BusesLayout graphLayout;
            graphLayout.inputBuses.add (juce::AudioChannelSet::stereo());
            graphLayout.outputBuses.add (juce::AudioChannelSet::stereo());
            processor.mainGraph->setBusesLayout (graphLayout);
        }

        using AudioGraphIOProcessor = juce::AudioProcessorGraph::AudioGraphIOProcessor;
        processor.audioInputNode  = processor.mainGraph->addNode (
            std::make_unique<AudioGraphIOProcessor> (AudioGraphIOProcessor::audioInputNode), {}, UK::none);
        processor.audioOutputNode = processor.mainGraph->addNode (
            std::make_unique<AudioGraphIOProcessor> (AudioGraphIOProcessor::audioOutputNode), {}, UK::none);
    }

    // Step 2: Pool-based node recycling
    struct PoolEntry {
        int slotId;
        int uid;
        size_t oldIndex;
        juce::AudioProcessorGraph::Node::Ptr node;
    };
    std::vector<PoolEntry> nodePool;
    for (size_t idx = 0; idx < processor.chainNodes.size(); ++idx)
    {
        if (processor.chainNodes[idx] != nullptr
            && idx < static_cast<size_t> (processor.currentPreset.numPlugins))
        {
            nodePool.push_back ({ processor.currentPreset.plugins[idx].slotId,
                                  processor.currentPreset.plugins[idx].uid,
                                  idx, processor.chainNodes[idx] });
        }
    }

    std::vector<bool> recycledFromOld (processor.chainNodes.size(), false);
    std::vector<size_t> recycledOldIndex;

    std::vector<juce::AudioProcessorGraph::Node::Ptr> newChainNodes;

    for (int i = 0; i < preset.numPlugins; ++i)
    {
        if (i >= kMaxPluginsInChain) break;

        auto& pluginCfg = const_cast<PluginConfig&> (preset.plugins[static_cast<size_t> (i)]);

        // First: try to recycle by slotId (exact instance match)
        juce::AudioProcessorGraph::Node::Ptr recycled;
        size_t matchedOldIdx = 0;
        if (pluginCfg.slotId > 0)
        {
            for (auto it = nodePool.begin(); it != nodePool.end(); ++it)
            {
                if (it->slotId == pluginCfg.slotId && it->uid == pluginCfg.uid)
                {
                    recycled = it->node;
                    matchedOldIdx = it->oldIndex;
                    nodePool.erase (it);
                    break;
                }
            }
        }

        // Fallback: recycle by UID
        if (recycled == nullptr)
        {
            for (auto it = nodePool.begin(); it != nodePool.end(); ++it)
            {
                if (it->uid == pluginCfg.uid)
                {
                    recycled = it->node;
                    matchedOldIdx = it->oldIndex;
                    if (pluginCfg.slotId == 0)
                        pluginCfg.slotId = it->slotId;
                    nodePool.erase (it);
                    break;
                }
            }
        }

        if (recycled != nullptr)
        {
            newChainNodes.push_back (recycled);
            recycledOldIndex.push_back (matchedOldIdx);

            if (pluginCfg.slotId == 0)
                pluginCfg.slotId = processor.nextSlotId++;

            juce::Logger::writeToLog ("Smart Rebuild: RECYCLE plugin[" + juce::String (i) + "] \""
                + recycled->getProcessor()->getName()
                + "\" uid=" + juce::String (pluginCfg.uid)
                + " slotId=" + juce::String (pluginCfg.slotId));
        }
        else
        {
            // Create fresh instance
            auto* targetDesc = processor.pluginCache->findPluginByUID (pluginCfg.uid);
            if (targetDesc == nullptr)
            {
                juce::Logger::writeToLog ("Plugin uid " + juce::String (pluginCfg.uid) + " not found in global cache!");
                recycledOldIndex.push_back (SIZE_MAX);
                continue;
            }

            juce::String err;
            auto instance = processor.pluginCache->createInstance (*targetDesc, sr, blockSz, err);
            if (instance == nullptr)
            {
                juce::Logger::writeToLog ("Failed to create plugin instance (uid " + juce::String (pluginCfg.uid) + "): " + err);
                recycledOldIndex.push_back (SIZE_MAX);
                continue;
            }

            // ── Stereo-only graph policy ──
            // All hosted plugins are forced to stereo layout. Mono plugins get
            // stereo layout or mono→stereo fan-out wiring as fallback.
            instance->enableAllBuses();
            {
                juce::AudioProcessor::BusesLayout pluginLayout;
                pluginLayout.inputBuses.add (juce::AudioChannelSet::stereo());
                pluginLayout.outputBuses.add (juce::AudioChannelSet::stereo());
                if (! instance->setBusesLayout (pluginLayout))
                    juce::Logger::writeToLog ("WARNING: setBusesLayout(stereo) failed for " + instance->getName());
            }

            pluginCfg.slotId = processor.nextSlotId++;

            juce::Logger::writeToLog ("Smart Rebuild: NEW plugin[" + juce::String (i) + "] \""
                + instance->getName()
                + "\" uid=" + juce::String (pluginCfg.uid)
                + " slotId=" + juce::String (pluginCfg.slotId));

            auto node = processor.mainGraph->addNode (std::move (instance), {}, UK::none);
            newChainNodes.push_back (node);
            node->setBypassed (false);
            recycledOldIndex.push_back (SIZE_MAX);
        }

        // DAW state restore for new + reused nodes
        if (isFromDAWLoad && preset.pluginStateBase64[static_cast<size_t> (i)].isNotEmpty())
        {
            auto& node = newChainNodes.back();
            juce::MemoryBlock stateBlock;
            if (stateBlock.fromBase64Encoding (preset.pluginStateBase64[static_cast<size_t> (i)]))
                node->getProcessor()->setStateInformation (stateBlock.getData(), static_cast<int> (stateBlock.getSize()));
        }
    }

    // Step 3: Remove orphaned nodes
    for (auto& entry : nodePool)
    {
        if (entry.node != nullptr)
        {
            juce::Logger::writeToLog ("Smart Rebuild: REMOVE orphan \""
                + entry.node->getProcessor()->getName()
                + "\" uid=" + juce::String (entry.uid)
                + " slotId=" + juce::String (entry.slotId));
            processor.mainGraph->removeNode (entry.node->nodeID, UK::none);
        }
    }

    // Step 4: Reconnect: Input → Node0 → ... → Output
    auto lastNode = processor.audioInputNode;
    for (auto& node : newChainNodes)
    {
        if (node != nullptr)
        {
            connectNodes (lastNode, node);
            lastNode = node;
        }
    }
    if (lastNode != nullptr && processor.audioOutputNode != nullptr)
        connectNodes (lastNode, processor.audioOutputNode);

    // Step 5: Update internal state
    processor.chainNodes = std::move (newChainNodes);

    juce::Logger::writeToLog ("Smart Rebuild complete: " + juce::String (static_cast<int> (processor.chainNodes.size()))
        + " plugins.  Connections: "
        + juce::String (static_cast<int> (processor.mainGraph->getConnections().size())));

    // Reconcile flags (bypass, userTouched, userOwned)
    {
        ChainPreset oldPreset = processor.currentPreset;
        processor.currentPreset = rebuildPreset;
        SlotState::reconcileFlags (oldPreset, processor.currentPreset,
                                   recycledOldIndex.data(), recycledOldIndex.size());
        SlotState::clearTrailingFlags (processor.currentPreset);
    }

    // ── Apply bypass/enable actions from server response ──
    {
        bool anyBypassAction = false;
        for (int i = 0; i < processor.currentPreset.numPlugins; ++i)
        {
            auto si = static_cast<size_t> (i);
            if (rebuildPreset.slotAction[si] == SlotActions::Bypass)
            {
                processor.currentPreset.bypassed[si] = true;
                anyBypassAction = true;
                juce::Logger::writeToLog ("performRebuild: SlotAction BYPASS slot[" + juce::String (i) + "]");
            }
            else if (rebuildPreset.slotAction[si] == SlotActions::Enable)
            {
                processor.currentPreset.bypassed[si] = false;
                anyBypassAction = true;
                juce::Logger::writeToLog ("performRebuild: SlotAction ENABLE slot[" + juce::String (i) + "]");
            }
        }

        bool anyBypassed = false;
        for (int i = 0; i < processor.currentPreset.numPlugins; ++i)
            if (processor.currentPreset.bypassed[static_cast<size_t> (i)]) { anyBypassed = true; break; }

        if (anyBypassed)
        {
            auto conns = processor.mainGraph->getConnections();
            for (auto& conn : conns)
                processor.mainGraph->removeConnection (conn, UK::none);

            auto lastN = processor.audioInputNode;
            for (int i = 0; i < static_cast<int> (processor.chainNodes.size()); ++i)
            {
                if (processor.currentPreset.bypassed[static_cast<size_t> (i)])
                    continue;
                if (processor.chainNodes[static_cast<size_t> (i)] != nullptr)
                {
                    connectNodes (lastN, processor.chainNodes[static_cast<size_t> (i)]);
                    lastN = processor.chainNodes[static_cast<size_t> (i)];
                }
            }
            if (lastN != nullptr && processor.audioOutputNode != nullptr)
                connectNodes (lastN, processor.audioOutputNode);

            juce::Logger::writeToLog ("performRebuild: bypass-aware rewire complete");
        }
    }

    // Apply parameters for non-DAW-load (new/reconfigured plugins)
    if (! isFromDAWLoad)
    {
        for (int i = 0; i < preset.numPlugins; ++i)
        {
            if (i >= static_cast<int> (processor.chainNodes.size())
                || processor.chainNodes[static_cast<size_t> (i)] == nullptr)
                continue;

            const auto& pc = preset.plugins[static_cast<size_t> (i)];
            if (pc.numParams == 0)
            {
                juce::Logger::writeToLog ("Smart Rebuild: SKIP params for main[" + juce::String (i)
                    + "] uid=" + juce::String (pc.uid) + " (retained, 0 params)");
                continue;
            }

            applyParamsToNode (processor.chainNodes[static_cast<size_t> (i)], pc, "main[" + juce::String (i) + "]");
        }
    }

    // ── Stereo-only graph policy ──
    processor.mainGraph->setPlayConfigDetails (2, 2, sr, blockSz);
    processor.mainGraph->prepareToPlay (sr, blockSz);

    juce::Logger::writeToLog ("Render sequence built.  Graph prepared at sr=" + juce::String (sr)
        + " block=" + juce::String (blockSz));

    processor.activeChainSize.store (static_cast<int> (processor.chainNodes.size()), std::memory_order_release);
    processor.isRebuildingGraph.store (false, std::memory_order_release);
    processor.suspendProcessing (false);

    // Remap automation pool
    processor.automationRouter.remap (processor.chainNodes, processor.currentPreset, processor.paramSlots);
    processor.updateHostDisplay (juce::AudioProcessor::ChangeDetails().withParameterInfoChanged (true));

    // Crossfade: smooth fade-in after rebuild
    processor.crossfadeTarget.store (1.0f, std::memory_order_relaxed);

    // Diagnostics (delayed probes)
    juce::Timer::callAfterDelay (1000, [&proc = processor]() {
        float pre  = proc.currentMetrics.diagPreRms.load (std::memory_order_relaxed);
        float post = proc.currentMetrics.diagPostRms.load (std::memory_order_relaxed);
        bool  byp  = proc.currentMetrics.diagGraphBypassed.load (std::memory_order_relaxed);
        float ag   = proc.currentMetrics.diagAutoGain.load (std::memory_order_relaxed);
        int   fifo = proc.currentMetrics.diagFifoApplied.load (std::memory_order_relaxed);
        int   pset = proc.currentMetrics.diagParamsSet.load (std::memory_order_relaxed);
        juce::Logger::writeToLog ("DIAG after 1s: preRms=" + juce::String (pre, 6)
            + " postRms=" + juce::String (post, 6)
            + " graphBypassed=" + juce::String (byp ? "YES" : "no")
            + " autoGain=" + juce::String (ag, 4)
            + " fifoPresets=" + juce::String (fifo)
            + " lastParamsSet=" + juce::String (pset)
            + " chainNodes=" + juce::String (static_cast<int> (proc.chainNodes.size()))
            + " diff=" + juce::String (post - pre, 6));
    });
    juce::Timer::callAfterDelay (3000, [&proc = processor]() {
        float pre  = proc.currentMetrics.diagPreRms.load (std::memory_order_relaxed);
        float post = proc.currentMetrics.diagPostRms.load (std::memory_order_relaxed);
        float ag   = proc.currentMetrics.diagAutoGain.load (std::memory_order_relaxed);
        int   fifo = proc.currentMetrics.diagFifoApplied.load (std::memory_order_relaxed);
        int   pset = proc.currentMetrics.diagParamsSet.load (std::memory_order_relaxed);
        juce::Logger::writeToLog ("DIAG after 3s: preRms=" + juce::String (pre, 6)
            + " postRms=" + juce::String (post, 6)
            + " autoGain=" + juce::String (ag, 4)
            + " fifoPresets=" + juce::String (fifo)
            + " lastParamsSet=" + juce::String (pset)
            + " chainNodes=" + juce::String (static_cast<int> (proc.chainNodes.size()))
            + " diff=" + juce::String (post - pre, 6));
    });

    // Modal guard DISABLED
    // modalGuard.start (30, 200);

    // Snapshot AI-applied parameters for user-intent detection
    processor.intentTracker.snapshotAIState (processor.chainNodes);

    // ── Slot-State Contract Diagnostics ──
    {
        int bypassedCount = 0, touchedCount = 0, ownedCount = 0;
        for (int i = 0; i < processor.currentPreset.numPlugins; ++i)
        {
            auto si = static_cast<size_t> (i);
            if (processor.currentPreset.bypassed[si])    ++bypassedCount;
            if (processor.currentPreset.userTouched[si]) ++touchedCount;
            if (processor.currentPreset.userOwned[si])   ++ownedCount;

            juce::String flags;
            if (processor.currentPreset.bypassed[si])    flags += "B";
            if (processor.currentPreset.userTouched[si]) flags += "T";
            if (processor.currentPreset.userOwned[si])   flags += "O";
            juce::Logger::writeToLog ("  slot[" + juce::String (i)
                + "] S" + juce::String (processor.currentPreset.plugins[si].slotId)
                + " uid=" + juce::String (processor.currentPreset.plugins[si].uid)
                + (flags.isNotEmpty() ? (" [" + flags + "]") : juce::String()));
        }
        juce::Logger::writeToLog ("SlotContract: " + juce::String (processor.currentPreset.numPlugins) + " plugins"
            + " | bypassed=" + juce::String (bypassedCount)
            + " touched=" + juce::String (touchedCount)
            + " owned=" + juce::String (ownedCount)
            + " | automationPool=" + juce::String (processor.automationRouter.getActiveMappedSlots()));
    }

    if (processor.onChainRebuilt)
        processor.onChainRebuilt();
}

// ─────────────────────────────────────────────────────────────────────────────
// applyBypassChanges: bypass/enable without topology rebuild
// ─────────────────────────────────────────────────────────────────────────────
void ChainGraphRebuilder::applyBypassChanges (const ChainPreset& preset)
{
    bool changed = false;
    for (int i = 0; i < preset.numPlugins && i < static_cast<int> (processor.chainNodes.size()); ++i)
    {
        auto si = static_cast<size_t> (i);
        uint8_t act = preset.slotAction[si];

        if (act == SlotActions::Bypass && ! processor.currentPreset.bypassed[si])
        {
            processor.currentPreset.bypassed[si] = true;
            changed = true;
            juce::Logger::writeToLog ("SlotAction: BYPASS slot[" + juce::String (i)
                + "] uid=" + juce::String (processor.currentPreset.plugins[si].uid));
        }
        else if (act == SlotActions::Enable && processor.currentPreset.bypassed[si])
        {
            processor.currentPreset.bypassed[si] = false;
            changed = true;
            juce::Logger::writeToLog ("SlotAction: ENABLE slot[" + juce::String (i)
                + "] uid=" + juce::String (processor.currentPreset.plugins[si].uid));
        }
    }

    if (changed)
        rewireGraphConnections();
}

// ─────────────────────────────────────────────────────────────────────────────
// rewireGraphConnections: bypass-aware graph wiring
// ─────────────────────────────────────────────────────────────────────────────
void ChainGraphRebuilder::rewireGraphConnections()
{
    if (processor.audioInputNode == nullptr || processor.audioOutputNode == nullptr)
        return;

    using UK = juce::AudioProcessorGraph::UpdateKind;

    processor.isRebuildingGraph.store (true, std::memory_order_release);

    auto connections = processor.mainGraph->getConnections();
    for (auto& conn : connections)
        processor.mainGraph->removeConnection (conn, UK::none);

    auto lastNode = processor.audioInputNode;

    for (int i = 0; i < static_cast<int> (processor.chainNodes.size()); ++i)
    {
        if (processor.currentPreset.bypassed[static_cast<size_t> (i)])
            continue;

        auto node = processor.chainNodes[static_cast<size_t> (i)];
        if (node == nullptr)
            continue;

        connectNodes (lastNode, node);
        lastNode = node;
    }

    connectNodes (lastNode, processor.audioOutputNode);

    const double sr = processor.getSampleRate() > 0 ? processor.getSampleRate() : 44100.0;
    const int blockSz = processor.getBlockSize() > 0 ? processor.getBlockSize() : 512;
    processor.mainGraph->prepareToPlay (sr, blockSz);

    processor.isRebuildingGraph.store (false, std::memory_order_release);
}
