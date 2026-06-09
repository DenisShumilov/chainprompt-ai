#pragma once

#include <JuceHeader.h>

class ChainPromptAudioProcessor;

// ── Drag-and-drop slot for a single plugin in the chain sidebar ──
class ChainSlotComponent : public juce::Component,
                           public juce::DragAndDropTarget
{
public:
    ChainSlotComponent (int index, const juce::String& name, bool bypassed, bool userTouched = false);

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

    int slotIndex = 0;

    std::function<void (int)> onClicked;
    std::function<void (int)> onDeleteClicked;
    std::function<void (int, bool)> onBypassChanged;
    std::function<void (int, int)> onReorder;

private:
    juce::Label nameLabel;
    juce::TextButton deleteBtn { "X" };
    juce::ToggleButton bypassBtn { "Byp" };
    bool dragOver = false;
    bool isUserTouched = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChainSlotComponent)
};

// ── Sidebar Presenter: owns chain slot widgets, manages sidebar content ──
class SidebarPresenter
{
public:
    SidebarPresenter (ChainPromptAudioProcessor& proc,
                      juce::Component& content,
                      juce::TextButton& addBtn);

    // Callbacks (set by editor after construction)
    std::function<void()> onClearHostedEditor;
    std::function<void (int)> onShowPluginUI;
    std::function<void (const juce::String&)> postToChat;

    /** Rebuild all chain slot widgets from current processor state. */
    void refresh();

    /** Show the add-plugin popup menu anchored to the add button. */
    void showAddPluginMenu();

    /** Position slots and add button inside the sidebar content area. */
    void layoutSlots (int sidebarWidth, int areaHeight);

    int getSlotCount() const { return chainSlots.size(); }

private:
    ChainPromptAudioProcessor& processor;
    juce::Component& sidebarContent;
    juce::TextButton& addButton;
    juce::OwnedArray<ChainSlotComponent> chainSlots;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SidebarPresenter)
};
