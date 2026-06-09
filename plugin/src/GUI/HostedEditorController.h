#pragma once

#include <JuceHeader.h>
#include "../Core/ChainPromptAudioProcessor.h"

/**
 * HostedEditorController — Manages the lifetime of a native plugin editor
 * embedded inside a scrollable viewport.
 *
 * Owns the unique_ptr<AudioProcessorEditor>; the editor holder and viewport
 * are owned by the main editor and passed by reference.
 */
class HostedEditorController
{
public:
    HostedEditorController (ChainPromptAudioProcessor& proc,
                            juce::Component& holder,
                            juce::Viewport& viewport)
        : processor (proc), editorHolder (holder), editorViewport (viewport)
    {
    }

    /** Show the native plugin UI for the chain node at `index`. */
    void show (int index)
    {
        clear();

        if (index < 0 || index >= static_cast<int> (processor.chainNodes.size()))
            return;

        auto node = processor.chainNodes[static_cast<size_t> (index)];
        if (node == nullptr)
            return;

        if (auto* proc = node->getProcessor())
        {
            if (proc->hasEditor())
            {
                hostedEditor.reset (proc->createEditorIfNeeded());

                if (hostedEditor != nullptr)
                {
                    editorHolder.addAndMakeVisible (hostedEditor.get());
                    editorHolder.setSize (hostedEditor->getWidth(), hostedEditor->getHeight());
                    hostedEditor->setTopLeftPosition (0, 0);
                    editorViewport.setViewPosition (0, 0);
                }
            }
        }
    }

    /** Destroy the currently hosted editor (if any). */
    void clear()
    {
        if (hostedEditor != nullptr)
        {
            editorHolder.removeChildComponent (hostedEditor.get());
            hostedEditor.reset();
        }
    }

private:
    ChainPromptAudioProcessor& processor;
    juce::Component& editorHolder;
    juce::Viewport& editorViewport;
    std::unique_ptr<juce::AudioProcessorEditor> hostedEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HostedEditorController)
};
