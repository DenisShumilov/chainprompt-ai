#include "SidebarPresenter.h"
#include "../Core/ChainPromptAudioProcessor.h"

// ═══════════════════════════════════════════════════════════════════════════════
// ChainSlotComponent
// ═══════════════════════════════════════════════════════════════════════════════

ChainSlotComponent::ChainSlotComponent (int index, const juce::String& name, bool bypassed, bool userTouched)
    : slotIndex (index), isUserTouched (userTouched)
{
    juce::String prefix = userTouched ? juce::String (index + 1) + "* "
                                       : juce::String (index + 1) + ". ";
    nameLabel.setText (prefix + name, juce::dontSendNotification);
    nameLabel.setColour (juce::Label::textColourId,
                         userTouched ? juce::Colour (0xFFFFB74D)
                                     : juce::Colour (0xFFE0E0E0));
    nameLabel.setFont (juce::FontOptions (13.0f));
    nameLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (nameLabel);

    deleteBtn.setWantsKeyboardFocus (false);
    deleteBtn.setColour (juce::TextButton::buttonColourId, juce::Colour (0xFF8B0000));
    deleteBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    deleteBtn.onClick = [this] {
        if (onDeleteClicked)
            onDeleteClicked (slotIndex);
    };
    addAndMakeVisible (deleteBtn);

    bypassBtn.setWantsKeyboardFocus (false);
    bypassBtn.setColour (juce::ToggleButton::textColourId, juce::Colour (0xFF999999));
    bypassBtn.setColour (juce::ToggleButton::tickColourId, juce::Colour (0xFFFF6B6B));
    bypassBtn.setToggleState (bypassed, juce::dontSendNotification);
    bypassBtn.onClick = [this] {
        if (onBypassChanged)
            onBypassChanged (slotIndex, bypassBtn.getToggleState());
    };
    addAndMakeVisible (bypassBtn);
}

void ChainSlotComponent::paint (juce::Graphics& g)
{
    if (dragOver)
    {
        g.setColour (juce::Colour (0xFF7C5CFC).withAlpha (0.3f));
        g.fillRect (getLocalBounds());
        g.setColour (juce::Colour (0xFF7C5CFC));
        g.drawRect (getLocalBounds(), 2);
    }
    else
    {
        g.setColour (juce::Colour (0xFF2A2D32));
        g.fillRect (getLocalBounds().reduced (1));

        if (isUserTouched)
        {
            g.setColour (juce::Colour (0xFFFFB74D));
            g.fillRect (0, 1, 3, getHeight() - 2);
        }
    }
}

void ChainSlotComponent::resized()
{
    auto b = getLocalBounds().reduced (2);
    deleteBtn.setBounds (b.removeFromRight (24).reduced (0, 2));
    b.removeFromRight (2);
    bypassBtn.setBounds (b.removeFromRight (50));
    nameLabel.setBounds (b);
}

void ChainSlotComponent::mouseDown (const juce::MouseEvent&)
{
    if (onClicked)
        onClicked (slotIndex);
}

void ChainSlotComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() > 5)
    {
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
            container->startDragging (juce::var (slotIndex), this);
    }
}

bool ChainSlotComponent::isInterestedInDragSource (const SourceDetails&) { return true; }

void ChainSlotComponent::itemDragEnter (const SourceDetails&)
{
    dragOver = true;
    repaint();
}

void ChainSlotComponent::itemDragExit (const SourceDetails&)
{
    dragOver = false;
    repaint();
}

void ChainSlotComponent::itemDropped (const SourceDetails& details)
{
    dragOver = false;
    repaint();

    int fromIndex = static_cast<int> (details.description);
    if (fromIndex != slotIndex && onReorder)
        onReorder (fromIndex, slotIndex);
}

// ═══════════════════════════════════════════════════════════════════════════════
// SidebarPresenter
// ═══════════════════════════════════════════════════════════════════════════════

SidebarPresenter::SidebarPresenter (ChainPromptAudioProcessor& proc,
                                    juce::Component& content,
                                    juce::TextButton& addBtn)
    : processor (proc), sidebarContent (content), addButton (addBtn)
{
    sidebarContent.addAndMakeVisible (addButton);
}

void SidebarPresenter::refresh()
{
    if (onClearHostedEditor)
        onClearHostedEditor();

    chainSlots.clear();

    for (int i = 0; i < static_cast<int> (processor.chainNodes.size()); ++i)
    {
        auto node = processor.chainNodes[static_cast<size_t> (i)];
        if (node == nullptr)
            continue;

        bool bypassed = processor.currentPreset.bypassed[static_cast<size_t> (i)];
        bool userTouched = processor.currentPreset.userTouched[static_cast<size_t> (i)];
        auto* slot = new ChainSlotComponent (i, node->getProcessor()->getName(), bypassed, userTouched);

        slot->onClicked = [this] (int idx) {
            if (onShowPluginUI)
                onShowPluginUI (idx);
        };

        slot->onDeleteClicked = [this] (int idx) {
            if (idx < 0 || idx >= static_cast<int> (processor.chainNodes.size()))
                return;

            processor.saveStateToUndo();

            ChainPreset newPreset;
            SlotState::buildPresetWithout (processor.currentPreset,
                                           static_cast<int> (processor.chainNodes.size()),
                                           idx, newPreset);

            processor.applyPresetTopology (newPreset);
        };

        slot->onBypassChanged = [this] (int idx, bool shouldBypass) {
            processor.setPluginBypassed (idx, shouldBypass);
        };

        slot->onReorder = [this] (int fromIdx, int toIdx) {
            int chainSize = static_cast<int> (processor.chainNodes.size());
            if (fromIdx < 0 || fromIdx >= chainSize || toIdx < 0 || toIdx >= chainSize)
                return;
            if (fromIdx == toIdx)
                return;

            processor.saveStateToUndo();

            struct SlotEntry { int uid; int slotId; bool bypassed; bool userTouched; bool userOwned; };
            std::vector<SlotEntry> entries;
            for (int s = 0; s < chainSize; ++s)
            {
                auto si = static_cast<size_t> (s);
                const auto& p = processor.currentPreset.plugins[si];
                entries.push_back ({ p.uid, p.slotId,
                                     processor.currentPreset.bypassed[si],
                                     processor.currentPreset.userTouched[si],
                                     processor.currentPreset.userOwned[si] });
            }

            SlotEntry moved = entries[static_cast<size_t> (fromIdx)];
            moved.userTouched = true;
            entries.erase (entries.begin() + fromIdx);
            entries.insert (entries.begin() + toIdx, moved);

            ChainPreset newPreset;
            newPreset.numPlugins = static_cast<int> (entries.size());
            for (int s = 0; s < newPreset.numPlugins && s < kMaxPluginsInChain; ++s)
            {
                auto si = static_cast<size_t> (s);
                newPreset.plugins[si].uid = entries[si].uid;
                newPreset.plugins[si].slotId = entries[si].slotId;
                newPreset.plugins[si].numParams = 0;
                newPreset.bypassed[si]    = entries[si].bypassed;
                newPreset.userTouched[si] = entries[si].userTouched;
                newPreset.userOwned[si]   = entries[si].userOwned;
            }

            processor.applyPresetTopology (newPreset);
        };

        chainSlots.add (slot);
        sidebarContent.addAndMakeVisible (slot);
    }
}

void SidebarPresenter::showAddPluginMenu()
{
    auto& cache = processor.getPluginCache();
    if (! cache.isReady())
    {
        if (postToChat)
            postToChat ("System: Plugin cache is still scanning. Please wait.\n\n");
        return;
    }

    if (static_cast<int> (processor.chainNodes.size()) >= kMaxPluginsInChain)
    {
        if (postToChat)
            postToChat ("System: Chain is full (" + juce::String (kMaxPluginsInChain) + " plugins max).\n\n");
        return;
    }

    const auto& plugins = cache.getPlugins();
    if (plugins.isEmpty())
        return;

    struct PluginMenuItem { juce::String name; int uid; };
    std::vector<PluginMenuItem> items;
    items.reserve (static_cast<size_t> (plugins.size()));

    for (auto* desc : plugins)
        items.push_back ({ desc->name, desc->uniqueId });

    std::sort (items.begin(), items.end(),
               [] (const PluginMenuItem& a, const PluginMenuItem& b) {
                   return a.name.compareIgnoreCase (b.name) < 0;
               });

    juce::PopupMenu menu;
    for (int i = 0; i < static_cast<int> (items.size()); ++i)
        menu.addItem (i + 1, items[static_cast<size_t> (i)].name);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (addButton)
                            .withMaximumNumColumns (1)
                            .withStandardItemHeight (22),
        [this, items] (int result) {
            if (result <= 0 || result > static_cast<int> (items.size()))
                return;

            int selectedUid = items[static_cast<size_t> (result - 1)].uid;

            processor.saveStateToUndo();

            ChainPreset newPreset = processor.currentPreset;
            int idx = newPreset.numPlugins;
            if (idx >= kMaxPluginsInChain)
                return;

            newPreset.plugins[static_cast<size_t> (idx)].uid = selectedUid;
            newPreset.plugins[static_cast<size_t> (idx)].numParams = 0;
            newPreset.bypassed[static_cast<size_t> (idx)] = false;
            newPreset.userOwned[static_cast<size_t> (idx)] = true;
            newPreset.pluginStateBase64[static_cast<size_t> (idx)] = {};
            newPreset.numPlugins = idx + 1;

            processor.applyPresetTopology (newPreset);
        });
}

void SidebarPresenter::layoutSlots (int sidebarWidth, int areaHeight)
{
    constexpr int slotH = 36;
    constexpr int addBtnH = 28;
    int contentH = chainSlots.size() * slotH + addBtnH + 4;
    sidebarContent.setBounds (0, 0, sidebarWidth - 10, juce::jmax (contentH, areaHeight));

    for (int i = 0; i < chainSlots.size(); ++i)
    {
        int y = i * slotH;
        chainSlots[i]->setBounds (2, y, sidebarContent.getWidth() - 4, slotH);
    }

    int addY = chainSlots.size() * slotH + 2;
    addButton.setBounds (2, addY, sidebarContent.getWidth() - 4, addBtnH);
}
