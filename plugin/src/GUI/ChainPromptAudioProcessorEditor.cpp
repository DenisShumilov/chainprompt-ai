#include "ChainPromptAudioProcessorEditor.h"


// ═══════════════════════════════════════════════════════════════════════════════
// ChainPromptAudioProcessorEditor
// ═══════════════════════════════════════════════════════════════════════════════

ChainPromptAudioProcessorEditor::ChainPromptAudioProcessorEditor (
    ChainPromptAudioProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      sidebarPresenter (p, sidebarContent, btnAddPlugin),
      hostedEditorCtrl (p, editorHolder, editorViewport),
      rescanController (p),
      uiRefreshCtrl (p, btnAnalyze, btnSend, btnUndo, btnRedo, btnRescan, chatDisplay)
{
    setSize (1400, 900);
    setResizable (true, true);
    setResizeLimits (1000, 600, 2400, 1600);
    setWantsKeyboardFocus (false);

    // Scrollable sidebar viewport
    addAndMakeVisible (sidebarViewport);
    sidebarViewport.setViewedComponent (&sidebarContent, false);
    sidebarViewport.setScrollBarsShown (true, false);

    // Scrollable viewport for embedded plugin editor
    addAndMakeVisible (editorViewport);
    editorViewport.setViewedComponent (&editorHolder, false);
    editorViewport.setScrollBarsShown (true, true);

    // --- Prompt box ---
    addAndMakeVisible (promptBox);
    promptBox.setMultiLine (true);
    promptBox.setReturnKeyStartsNewLine (false);
    promptBox.setWantsKeyboardFocus (true);
    promptBox.setTextToShowWhenEmpty ("Describe what you want for your mix...",
                                      juce::Colour (0xFF6B6B6B));
    promptBox.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xFF2A2D32));
    promptBox.setColour (juce::TextEditor::textColourId, juce::Colour (0xFFE0E0E0));
    promptBox.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xFF3A3D44));
    promptBox.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour (0xFF7C5CFC));
    promptBox.setFont (juce::FontOptions (15.0f));

    auto styleButton = [] (juce::TextButton& btn, juce::Colour bgColour)
    {
        btn.setWantsKeyboardFocus (false);
        btn.setColour (juce::TextButton::buttonColourId, bgColour);
        btn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    };

    // ── Analyze Audio button ──
    addAndMakeVisible (btnAnalyze);
    styleButton (btnAnalyze, juce::Colour (0xFF3A3D44));
    btnAnalyze.onClick = [this] {
        auto state = audioProcessor.captureManager.captureState.load (std::memory_order_relaxed);

        if (state == CaptureState::Idle)
        {
            double sr = audioProcessor.getSampleRate();
            if (sr <= 0) sr = 44100.0;
            int totalSamples = static_cast<int> (sr * 4.0);
            int numCh = audioProcessor.getTotalNumInputChannels();
            if (numCh < 1) numCh = 2;

            audioProcessor.captureManager.captureBuffer.setSize (numCh, totalSamples, false, true, false);
            audioProcessor.captureManager.captureTotalSamples.store (totalSamples, std::memory_order_relaxed);
            audioProcessor.captureManager.captureWritePosition.store (0, std::memory_order_relaxed);
            audioProcessor.captureManager.capturedAudioBase64 = {};
            audioProcessor.captureManager.capturedAudioMimeType = {};
            captureNotified = false;
            audioProcessor.captureManager.captureState.store (CaptureState::Waiting, std::memory_order_relaxed);

            btnAnalyze.setButtonText ("Waiting for playback...");
            btnAnalyze.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFFB71C1C));
        }
        else if (state == CaptureState::Done)
        {
            audioProcessor.captureManager.captureState.store (CaptureState::Idle, std::memory_order_relaxed);
            audioProcessor.captureManager.capturedAudioBase64 = {};
            audioProcessor.captureManager.capturedAudioMimeType = {};
            captureNotified = false;
            btnAnalyze.setButtonText ("Analyze Audio");
            btnAnalyze.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFF3A3D44));
        }
    };

    // ── Send button ──
    addAndMakeVisible (btnSend);
    styleButton (btnSend, juce::Colour (0xFF7C5CFC));
    btnSend.onClick = [this] {
        juce::String userText = promptBox.getText().trim();
        if (userText.isEmpty())
        {
            chatDisplay.moveCaretToEnd();
            chatDisplay.insertTextAtCaret ("System: Please enter a prompt first.\n\n");
            return;
        }

        chatDisplay.moveCaretToEnd();
        chatDisplay.insertTextAtCaret ("You: " + userText + "\n\n");
        promptBox.clear();

        audioProcessor.detectUserTouches();

        {
            std::vector<LLMCoordinator::ChainSlotInfo> chainEntries;
            const auto& cp = audioProcessor.currentPreset;
            for (int i = 0; i < cp.numPlugins; ++i)
            {
                auto si = static_cast<size_t> (i);
                chainEntries.push_back ({ cp.plugins[si].uid,
                                          cp.plugins[si].slotId,
                                          cp.userTouched[si],
                                          cp.bypassed[si],
                                          cp.userOwned[si] });
            }
            audioProcessor.llmCoordinator.setCurrentChain (chainEntries);
        }

        audioProcessor.llmCoordinator.setInstanceInfo (audioProcessor.instanceId,
                                                        audioProcessor.trackName,
                                                        audioProcessor.projectScope);

        if (audioProcessor.captureManager.capturedAudioPath.isNotEmpty()
            || audioProcessor.captureManager.capturedAudioBase64.isNotEmpty())
        {
            const auto& fm = audioProcessor.captureManager.frozenMetrics;

            std::array<float, 31> spec31 {};
            if (fm.spectrum31Ready)
            {
                for (int i = 0; i < 31; ++i)
                    spec31[static_cast<size_t> (i)] = fm.spectrum31[static_cast<size_t> (i)];
            }

            audioProcessor.llmCoordinator.submitPromptWithAudio (
                userText,
                fm.rmsDB, fm.peakDB, fm.crestFactor, fm.zcr, fm.bpm,
                fm.subPct, fm.lowPct, fm.lowMidPct, fm.highMidPct, fm.airPct,
                fm.isStereo,
                audioProcessor.captureManager.capturedAudioBase64,
                audioProcessor.captureManager.capturedAudioMimeType,
                spec31,
                fm.mdTserDb,
                fm.spectralFlux,
                fm.oeHerDb,
                fm.isTonal,
                fm.bwIcc,
                fm.lrBalanceDb,
                audioProcessor.captureManager.capturedAudioPath);
        }
        else
        {
            audioProcessor.llmCoordinator.submitPrompt (userText);
        }
    };

    // ── New Chat button ──
    addAndMakeVisible (btnNewChat);
    btnNewChat.setWantsKeyboardFocus (false);
    btnNewChat.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFF3A3D44));
    btnNewChat.setColour (juce::TextButton::textColourOffId, juce::Colour (0xFFE0E0E0));
    btnNewChat.onClick = [this] {
        chatDisplay.clear();
        audioProcessor.saveStateToUndo();
        ChainPreset emptyPreset;
        emptyPreset.numPlugins = 0;
        audioProcessor.applyPresetTopology (emptyPreset);

        audioProcessor.captureManager.captureState.store (CaptureState::Idle, std::memory_order_relaxed);
        audioProcessor.captureManager.capturedAudioBase64 = {};
        audioProcessor.captureManager.capturedAudioPath = {};
        captureNotified = false;
        btnAnalyze.setButtonText ("Analyze Audio");
        btnAnalyze.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFF3A3D44));

        audioProcessor.llmCoordinator.clearChatHistory();
    };

    // ── Undo / Redo buttons ──
    addAndMakeVisible (btnUndo);
    styleButton (btnUndo, juce::Colour (0xFF3A3D44));
    btnUndo.setEnabled (false);
    btnUndo.onClick = [this] { audioProcessor.undo(); };

    addAndMakeVisible (btnRedo);
    styleButton (btnRedo, juce::Colour (0xFF3A3D44));
    btnRedo.setEnabled (false);
    btnRedo.onClick = [this] { audioProcessor.redo(); };

    // ── Chat message callback ──
    audioProcessor.llmCoordinator.onChatMessage = [this] (const juce::String& role,
                                                           const juce::String& message)
    {
        chatDisplay.moveCaretToEnd();
        chatDisplay.insertTextAtCaret (role + ": " + message + "\n\n");
    };

    // ── Auto-Gain Match toggle ──
    addAndMakeVisible (btnAutoGain);
    btnAutoGain.setWantsKeyboardFocus (false);
    btnAutoGain.setColour (juce::ToggleButton::textColourId, juce::Colour (0xFFE0E0E0));
    btnAutoGain.setColour (juce::ToggleButton::tickColourId, juce::Colour (0xFF7C5CFC));
    btnAutoGain.setToggleState (audioProcessor.autoGainEnabled.load(), juce::dontSendNotification);
    btnAutoGain.onClick = [this] {
        audioProcessor.autoGainEnabled.store (btnAutoGain.getToggleState());
    };

    // ── Chat display ──
    addAndMakeVisible (chatDisplay);
    chatDisplay.setMultiLine (true);
    chatDisplay.setReadOnly (true);
    chatDisplay.setScrollbarsShown (true);
    chatDisplay.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xFF1E2024));
    chatDisplay.setColour (juce::TextEditor::textColourId, juce::Colour (0xFFD0D0D0));
    chatDisplay.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    chatDisplay.setFont (juce::FontOptions (14.0f));
    chatDisplay.setWantsKeyboardFocus (false);

    // ── Add Plugin button (styling; logic wired via SidebarPresenter) ──
    btnAddPlugin.setWantsKeyboardFocus (false);
    btnAddPlugin.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFF2E7D32));
    btnAddPlugin.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    btnAddPlugin.onClick = [this] { sidebarPresenter.showAddPluginMenu(); };

    // ── Rescan button (delegates to RescanWorkflowController) ──
    addAndMakeVisible (btnRescan);
    btnRescan.setWantsKeyboardFocus (false);
    btnRescan.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFF3A3D44));
    btnRescan.setColour (juce::TextButton::textColourOffId, juce::Colour (0xFFE0E0E0));
    btnRescan.onClick = [this] { rescanController.startRescan(); };

    // ── Wire controller callbacks ──
    auto postToChat = [this] (const juce::String& msg) {
        chatDisplay.moveCaretToEnd();
        chatDisplay.insertTextAtCaret (msg);
    };

    sidebarPresenter.onClearHostedEditor = [this] { hostedEditorCtrl.clear(); };
    sidebarPresenter.onShowPluginUI = [this] (int idx) { hostedEditorCtrl.show (idx); };
    sidebarPresenter.postToChat = postToChat;

    rescanController.postToChat = postToChat;
    rescanController.onRefreshSidebar = [this] {
        sidebarPresenter.refresh();
        resized();
    };

    // ── Initial state ──
    promptBox.setEnabled (true);

    sidebarPresenter.refresh();
    refreshChatUI();

    startTimerHz (15);

    audioProcessor.onChainRebuilt = [this] {
        sidebarPresenter.refresh();
        resized();
    };
}

ChainPromptAudioProcessorEditor::~ChainPromptAudioProcessorEditor()
{
    audioProcessor.onChainRebuilt = nullptr;
    audioProcessor.llmCoordinator.onChatMessage = nullptr;

    stopTimer();
    hostedEditorCtrl.clear();
}

void ChainPromptAudioProcessorEditor::clearHostedEditor()
{
    hostedEditorCtrl.clear();
}

void ChainPromptAudioProcessorEditor::refreshChatUI()
{
    chatDisplay.clear();

    auto history = audioProcessor.llmCoordinator.getChatHistory();

    for (const auto& entry : history)
    {
        juce::String prefix = (entry.role == "user") ? "You" : "AI";
        juce::String text = entry.uiText.isNotEmpty() ? entry.uiText : entry.rawText;
        chatDisplay.moveCaretToEnd();
        chatDisplay.insertTextAtCaret (prefix + ": " + text + "\n\n");
    }

    chatDisplay.moveCaretToEnd();
}

void ChainPromptAudioProcessorEditor::timerCallback()
{
    uiRefreshCtrl.tick (captureNotified);

    // Continuous: repaint analyzer strip
    repaint (0, getHeight() - 32, getWidth(), 32);
}

void ChainPromptAudioProcessorEditor::paint (juce::Graphics& g)
{
    const int analyzerH = 32;
    const int bottomY = getHeight() - analyzerH;

    g.fillAll (juce::Colour (0xFF1A1C20));

    // Header bar
    g.setColour (juce::Colour (0xFF222529));
    g.fillRect (0, 0, getWidth(), 60);

    g.setColour (juce::Colour (0xFFE0E0E0));
    g.setFont (juce::FontOptions (20.0f));
    g.drawText ("ChainPrompt AI", 20, 8, 250, 24, juce::Justification::centredLeft);

    // Left sidebar background
    g.setColour (juce::Colour (0xFF222529));
    g.fillRect (0, 60, 160, bottomY - 60);
    g.setColour (juce::Colour (0xFF3A3D44));
    g.drawVerticalLine (160, 60.0f, static_cast<float> (bottomY));

    // Right sidebar background (chat panel)
    int rightSidebarX = getWidth() - 300;
    g.setColour (juce::Colour (0xFF222529));
    g.fillRect (rightSidebarX, 60, 300, bottomY - 60);
    g.setColour (juce::Colour (0xFF3A3D44));
    g.drawVerticalLine (rightSidebarX, 60.0f, static_cast<float> (bottomY));

    // Analyzer strip at bottom
    g.setColour (juce::Colour (0xFF16181B));
    g.fillRect (0, bottomY, getWidth(), analyzerH);
    g.setColour (juce::Colour (0xFF3A3D44));
    g.drawHorizontalLine (bottomY, 0.0f, static_cast<float> (getWidth()));

    const float rms   = audioProcessor.currentMetrics.rmsDB.load (std::memory_order_relaxed);
    const float peak  = audioProcessor.currentMetrics.peakDB.load (std::memory_order_relaxed);
    const float crest = audioProcessor.currentMetrics.crestFactor.load (std::memory_order_relaxed);
    const float zcr   = audioProcessor.currentMetrics.zcr.load (std::memory_order_relaxed);
    const float sub   = audioProcessor.currentMetrics.subPct.load (std::memory_order_relaxed);
    const float low   = audioProcessor.currentMetrics.lowPct.load (std::memory_order_relaxed);
    const float lmid  = audioProcessor.currentMetrics.lowMidPct.load (std::memory_order_relaxed);
    const float hmid  = audioProcessor.currentMetrics.highMidPct.load (std::memory_order_relaxed);
    const float air   = audioProcessor.currentMetrics.airPct.load (std::memory_order_relaxed);

    g.setFont (juce::FontOptions (11.0f));

    auto stripBounds = juce::Rectangle<int> (8, bottomY + 2, getWidth() - 16, analyzerH - 4);

    auto drawMetric = [&] (const juce::String& label, const juce::String& value, int width)
    {
        auto cell = stripBounds.removeFromLeft (width);
        g.setColour (juce::Colour (0xFF6B6B6B));
        g.drawText (label, cell.removeFromLeft (cell.getWidth() / 2), juce::Justification::centredRight);
        g.setColour (juce::Colour (0xFF7C5CFC));
        g.drawText (value, cell, juce::Justification::centredLeft);
    };

    drawMetric ("RMS ", juce::String (rms, 1) + " dB", 120);
    drawMetric ("Peak ", juce::String (peak, 1) + " dB", 120);
    drawMetric ("Crest ", juce::String (crest, 1) + " dB", 110);
    drawMetric ("ZCR ", juce::String (zcr, 3), 90);

    stripBounds.removeFromLeft (8);
    g.setColour (juce::Colour (0xFF3A3D44));
    g.drawVerticalLine (stripBounds.getX(), static_cast<float> (bottomY + 6),
                        static_cast<float> (bottomY + analyzerH - 6));
    stripBounds.removeFromLeft (8);

    drawMetric ("Sub ", juce::String (sub, 1) + "%", 90);
    drawMetric ("Low ", juce::String (low, 1) + "%", 90);
    drawMetric ("LMid ", juce::String (lmid, 1) + "%", 100);
    drawMetric ("HMid ", juce::String (hmid, 1) + "%", 100);
    drawMetric ("Air ", juce::String (air, 1) + "%", 90);

    // Loading overlay
    if (! audioProcessor.isPluginCacheReady())
    {
        g.setColour (juce::Colour (0xFF1A1C20).withAlpha (0.92f));
        g.fillAll();

        g.setColour (juce::Colour (0xFF7C5CFC));
        g.setFont (juce::FontOptions (22.0f));
        g.drawFittedText ("Scanning Waves Ecosystem...\nPlease wait.",
                          getLocalBounds(), juce::Justification::centred, 2);
    }
}

void ChainPromptAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();

    // Analyzer strip at bottom (32px)
    bounds.removeFromBottom (32);

    // Header bar (top 60px) — Rescan button lives in the header
    btnRescan.setBounds (getWidth() - 100, 18, 80, 24);

    bounds.removeFromTop (60);

    // Left sidebar: 160px with scrollable viewport
    constexpr int sidebarW = 160;
    auto leftSidebarArea = bounds.removeFromLeft (sidebarW);
    sidebarViewport.setBounds (leftSidebarArea);

    // Delegate slot layout to SidebarPresenter
    sidebarPresenter.layoutSlots (sidebarW, leftSidebarArea.getHeight());

    // Right sidebar: 300px (chat panel)
    auto rightSidebar = bounds.removeFromRight (300).reduced (6);

    auto promptArea    = rightSidebar.removeFromBottom (60);
    auto sendArea      = rightSidebar.removeFromBottom (30);
    auto analyzeArea   = rightSidebar.removeFromBottom (30);
    auto newChatArea   = rightSidebar.removeFromTop (30);
    auto undoRedoArea  = rightSidebar.removeFromTop (28);
    auto autoGainArea  = rightSidebar.removeFromTop (28);

    promptBox.setBounds (promptArea);
    btnSend.setBounds (sendArea.reduced (0, 2));
    btnAnalyze.setBounds (analyzeArea.reduced (0, 2));
    btnNewChat.setBounds (newChatArea.reduced (0, 2));
    {
        auto undoRedoReduced = undoRedoArea.reduced (0, 2);
        int halfW = undoRedoReduced.getWidth() / 2;
        btnUndo.setBounds (undoRedoReduced.removeFromLeft (halfW - 2));
        undoRedoReduced.removeFromLeft (4);
        btnRedo.setBounds (undoRedoReduced);
    }
    btnAutoGain.setBounds (autoGainArea.reduced (0, 2));

    chatDisplay.setBounds (rightSidebar);

    // Center: embedded plugin editor viewport
    editorViewport.setBounds (bounds);
}
