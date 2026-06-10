#pragma once

#include <JuceHeader.h>
#include "../Core/ChainPromptAudioProcessor.h"
#include "ChainPromptLookAndFeel.h"
#include "SidebarPresenter.h"
#include "RescanWorkflowController.h"
#include "HostedEditorController.h"
#include "UIRefreshController.h"

// ── Main Editor (composition root) ──
class ChainPromptAudioProcessorEditor : public juce::AudioProcessorEditor,
                                        public juce::DragAndDropContainer,
                                        public juce::Timer
{
public:
    explicit ChainPromptAudioProcessorEditor (ChainPromptAudioProcessor&);
    ~ChainPromptAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void timerCallback() override;
    void refreshChatUI();

    /** Public: called by ChainGraphRebuilder before topology rebuild. */
    void clearHostedEditor();

private:
    // Declared first so it is destroyed last; the destructor clears the
    // editor's LookAndFeel pointer before this object goes away.
    ChainPromptLookAndFeel lookAndFeel;

    ChainPromptAudioProcessor& audioProcessor;

    // ── Chat & prompt controls ──
    juce::TextEditor promptBox;
    juce::TextButton btnAnalyze { "Analyze Audio" };
    juce::TextButton btnSend    { "Send" };
    juce::TextButton btnNewChat { "New Chat" };
    juce::TextButton btnUndo    { "Undo" };
    juce::TextButton btnRedo    { "Redo" };
    juce::ToggleButton btnAutoGain { "Auto-Gain Match" };
    juce::TextEditor chatDisplay;

    bool captureNotified = false;

    // ── Rescan button (lives in header bar) ──
    juce::TextButton btnRescan { "Rescan" };

    // ── Sidebar visual containers (owned here, managed by SidebarPresenter) ──
    juce::Viewport sidebarViewport;
    juce::Component sidebarContent;
    juce::TextButton btnAddPlugin { "+" };

    // ── Hosted editor visual containers (owned here, managed by HostedEditorController) ──
    juce::Viewport editorViewport;
    juce::Component editorHolder;

    // ── Extracted controllers (declared AFTER the components they reference) ──
    SidebarPresenter         sidebarPresenter;
    HostedEditorController   hostedEditorCtrl;
    RescanWorkflowController rescanController;
    UIRefreshController      uiRefreshCtrl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChainPromptAudioProcessorEditor)
};
