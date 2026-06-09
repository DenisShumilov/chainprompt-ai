#pragma once

#include <JuceHeader.h>
#include "../Core/ChainPromptAudioProcessor.h"

/**
 * UIRefreshController — Periodic UI polling logic extracted from the editor's
 * timerCallback.  Updates button states, capture progress, AI thinking
 * indicator, undo/redo availability, and backend health diagnostics.
 *
 * Does NOT inherit Timer — the editor owns the timer and calls tick().
 */
class UIRefreshController
{
public:
    UIRefreshController (ChainPromptAudioProcessor& p,
                         juce::TextButton& analyze,
                         juce::TextButton& send,
                         juce::TextButton& undo,
                         juce::TextButton& redo,
                         juce::TextButton& rescan,
                         juce::TextEditor& chat)
        : processor (p),
          btnAnalyze (analyze), btnSend (send),
          btnUndo (undo), btnRedo (redo),
          btnRescan (rescan), chatDisplay (chat)
    {
    }

    /** Called from the editor's timerCallback at 15 Hz. */
    void tick (bool& captureNotified)
    {
        // ── Analyze button state machine ──
        auto capState = processor.captureManager.captureState.load (std::memory_order_relaxed);

        if (capState == CaptureState::Recording)
        {
            int pos   = processor.captureManager.captureWritePosition.load (std::memory_order_relaxed);
            int total = processor.captureManager.captureTotalSamples.load (std::memory_order_relaxed);
            int pct   = (total > 0) ? (pos * 100 / total) : 0;
            btnAnalyze.setButtonText ("Recording... " + juce::String (pct) + "%");
            btnAnalyze.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFFB71C1C));
        }
        else if (capState == CaptureState::Done)
        {
            if (! captureNotified)
            {
                captureNotified = true;
                chatDisplay.moveCaretToEnd();
                chatDisplay.insertTextAtCaret ("System: Audio captured and analyzed. Ready to send request!\n\n");
            }

            btnAnalyze.setButtonText ("Audio Captured");
            btnAnalyze.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFF2E7D32));
        }
        else if (capState == CaptureState::Waiting)
        {
            btnAnalyze.setButtonText ("Waiting for playback...");
            btnAnalyze.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFFB71C1C));
        }
        else if (capState == CaptureState::Idle)
        {
            if (btnAnalyze.getButtonText() != "1. Analyze Audio")
            {
                btnAnalyze.setButtonText ("Analyze Audio");
                btnAnalyze.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFF3A3D44));
            }
        }

        // ── Backend health diagnostics ──
        auto currentBackendState = processor.healthChecker.getState();
        if (currentBackendState != lastBackendState)
        {
            lastBackendState = currentBackendState;
            postBackendStateMessage (currentBackendState);
        }

        // ── "Thinking..." indicator + backend availability ──
        bool aiThinking = processor.llmCoordinator.isProcessing.load (std::memory_order_relaxed);
        bool backendDown = (currentBackendState == BackendState::Unreachable);

        if (aiThinking)
        {
            if (btnSend.isEnabled())
            {
                btnSend.setEnabled (false);
                btnSend.setButtonText ("Thinking...");
                btnSend.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFF4A4D54));
            }
        }
        else if (backendDown)
        {
            if (btnSend.isEnabled() || btnSend.getButtonText() != "Backend Offline")
            {
                btnSend.setEnabled (false);
                btnSend.setButtonText ("Backend Offline");
                btnSend.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFFB71C1C));
            }
        }
        else
        {
            if (! btnSend.isEnabled() || btnSend.getButtonText() != "Send")
            {
                btnSend.setEnabled (true);
                btnSend.setButtonText ("Send");
                btnSend.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFF7C5CFC));
            }
        }

        // ── Undo/Redo enable/disable ──
        btnUndo.setEnabled (processor.canUndo());
        btnRedo.setEnabled (processor.canRedo());

        // ── Rescan button state ──
        btnRescan.setEnabled (processor.isPluginCacheReady());
    }

private:
    void postBackendStateMessage (BackendState newState)
    {
        juce::String msg;

        switch (newState)
        {
            case BackendState::Unreachable:
                msg = "System: Backend unreachable at 127.0.0.1:5000.\n"
                      "  Start the backend: python server.py\n"
                      "  Then verify: GET http://127.0.0.1:5000/health\n\n";
                break;

            case BackendState::NoInventory:
                msg = "System: Backend is running but plugin inventory is not available.\n"
                      "  Please click Rescan to scan your plugins.\n\n";
                break;

            case BackendState::Ready:
                msg = "System: Backend connected and ready.\n\n";
                break;

            case BackendState::Unknown:
            default:
                break;
        }

        if (msg.isNotEmpty())
        {
            chatDisplay.moveCaretToEnd();
            chatDisplay.insertTextAtCaret (msg);
        }
    }

    ChainPromptAudioProcessor& processor;
    juce::TextButton& btnAnalyze;
    juce::TextButton& btnSend;
    juce::TextButton& btnUndo;
    juce::TextButton& btnRedo;
    juce::TextButton& btnRescan;
    juce::TextEditor& chatDisplay;

    BackendState lastBackendState = BackendState::Unknown;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UIRefreshController)
};
