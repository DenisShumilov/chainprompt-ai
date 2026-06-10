#pragma once

/**
 * ChainPromptLookAndFeel — shared visual theme for the editor.
 *
 * Palette follows the project's brand deck: deep navy surfaces, teal brand
 * accent, purple as the primary-action colour. Pure styling: no behavioural
 * overrides, no component logic. The editor owns one instance and must call
 * setLookAndFeel(nullptr) before this object is destroyed.
 */

#include <JuceHeader.h>

namespace cp_theme
{
    inline const juce::Colour bgDeep      { 0xFF0E141B };  // window background
    inline const juce::Colour bgPanel     { 0xFF141B23 };  // side panels / header
    inline const juce::Colour bgStrip     { 0xFF0B1016 };  // analyzer strip
    inline const juce::Colour surface     { 0xFF1D2733 };  // buttons, cards
    inline const juce::Colour surfaceHi   { 0xFF26323F };  // hover
    inline const juce::Colour field       { 0xFF10171F };  // text fields
    inline const juce::Colour hairline    { 0xFF26303B };  // outlines / dividers
    inline const juce::Colour teal        { 0xFF3DDCA3 };  // brand accent
    inline const juce::Colour purple      { 0xFF7C5CFC };  // primary action
    inline const juce::Colour textHi      { 0xFFE8EDF2 };
    inline const juce::Colour textLo      { 0xFF8A97A5 };
    inline const juce::Colour amber       { 0xFFFFB74D };  // user-touched marker
    inline const juce::Colour danger      { 0xFFB3404C };  // delete / destructive
}

class ChainPromptLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ChainPromptLookAndFeel()
    {
        setColour (juce::TextButton::buttonColourId,   cp_theme::surface);
        setColour (juce::TextButton::textColourOffId,  cp_theme::textHi);
        setColour (juce::TextButton::textColourOnId,   cp_theme::textHi);

        setColour (juce::TextEditor::backgroundColourId,     cp_theme::field);
        setColour (juce::TextEditor::textColourId,           cp_theme::textHi);
        setColour (juce::TextEditor::outlineColourId,        cp_theme::hairline);
        setColour (juce::TextEditor::focusedOutlineColourId, cp_theme::teal);
        setColour (juce::TextEditor::highlightColourId,      cp_theme::purple.withAlpha (0.35f));
        setColour (juce::CaretComponent::caretColourId,      cp_theme::teal);

        setColour (juce::ToggleButton::textColourId, cp_theme::textHi);
        setColour (juce::ToggleButton::tickColourId, cp_theme::teal);

        setColour (juce::ScrollBar::thumbColourId, cp_theme::surfaceHi);

        setColour (juce::PopupMenu::backgroundColourId,            cp_theme::bgPanel);
        setColour (juce::PopupMenu::textColourId,                  cp_theme::textHi);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, cp_theme::surfaceHi);
        setColour (juce::PopupMenu::highlightedTextColourId,       cp_theme::textHi);

        setColour (juce::Label::textColourId, cp_theme::textHi);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool isHighlighted, bool isDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        auto base = backgroundColour;

        if (! button.isEnabled())
            base = base.withAlpha (0.45f);
        else if (isDown)
            base = base.darker (0.25f);
        else if (isHighlighted)
            base = base.brighter (0.18f);

        constexpr float corner = 6.0f;
        g.setColour (base);
        g.fillRoundedRectangle (bounds, corner);

        g.setColour (cp_theme::hairline.withAlpha (button.isEnabled() ? 1.0f : 0.5f));
        g.drawRoundedRectangle (bounds, corner, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return juce::Font (juce::FontOptions (juce::jmin (14.0f, buttonHeight * 0.55f),
                                              juce::Font::bold));
    }

    void drawTickBox (juce::Graphics& g, juce::Component& component,
                      float x, float y, float w, float h,
                      bool ticked, bool isEnabled,
                      bool /*highlighted*/, bool /*down*/) override
    {
        juce::ignoreUnused (component);
        auto box = juce::Rectangle<float> (x, y, w, h).reduced (1.5f);

        g.setColour (cp_theme::field);
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (ticked ? cp_theme::teal : cp_theme::hairline);
        g.drawRoundedRectangle (box, 4.0f, 1.2f);

        if (ticked)
        {
            g.setColour (isEnabled ? cp_theme::teal : cp_theme::teal.withAlpha (0.5f));
            auto tick = box.reduced (box.getWidth() * 0.28f);
            juce::Path p;
            p.startNewSubPath (tick.getX(), tick.getCentreY() + tick.getHeight() * 0.1f);
            p.lineTo (tick.getCentreX() - tick.getWidth() * 0.1f, tick.getBottom());
            p.lineTo (tick.getRight(), tick.getY());
            g.strokePath (p, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        }
    }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar& bar,
                        int x, int y, int width, int height,
                        bool isVertical, int thumbStart, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override
    {
        juce::ignoreUnused (bar);
        auto thumb = isVertical
            ? juce::Rectangle<int> (x + 2, thumbStart, width - 4, thumbSize)
            : juce::Rectangle<int> (thumbStart, y + 2, thumbSize, height - 4);

        g.setColour (cp_theme::surfaceHi.withAlpha ((isMouseOver || isMouseDown) ? 1.0f : 0.6f));
        g.fillRoundedRectangle (thumb.toFloat(), 3.0f);
    }
};
