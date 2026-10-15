#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// Theme colours. These are mutable so the whole UI can flip between ICE (navy/cyan) and MIDNIGHT (80s synthwave).
namespace IceColours
{
    inline juce::Colour bg        { 0xff0a0d14 };
    inline juce::Colour panel     { 0xff121724 };
    inline juce::Colour panelEdge { 0xff1f2838 };
    inline juce::Colour text      { 0xffc9d6e6 };
    inline juce::Colour dimText   { 0xff6b7a90 };
    inline juce::Colour ice       { 0xff6fe3ff };
    inline juce::Colour iceDeep   { 0xff2b8cff };
    inline juce::Colour iceGlow   { 0xffb7f4ff };
    inline juce::Colour knobBody  { 0xff1a2130 };
    inline juce::Colour knobRing  { 0xff262f42 };
    inline juce::Colour screen    { 0xff070a10 };   // scope / env / drag pad backgrounds
    inline int          theme     = 0;             // 0 = ICE, 1 = MIDNIGHT

    inline void applyTheme (int which)
    {
        theme = which;
        if (which == 1)
        {
            // MIDNIGHT: deep violet night, hot magenta accents, sunset glow
            bg        = juce::Colour (0xff0b0614);
            panel     = juce::Colour (0xff150c24);
            panelEdge = juce::Colour (0xff3a1d5c);
            text      = juce::Colour (0xfff3dcff);
            dimText   = juce::Colour (0xff9a78b8);
            ice       = juce::Colour (0xffff3fa4);
            iceDeep   = juce::Colour (0xff8a2be2);
            iceGlow   = juce::Colour (0xffffc46b);
            knobBody  = juce::Colour (0xff1e1030);
            knobRing  = juce::Colour (0xff2e1a48);
            screen    = juce::Colour (0xff0a0512);
        }
        else
        {
            bg        = juce::Colour (0xff0a0d14);
            panel     = juce::Colour (0xff121724);
            panelEdge = juce::Colour (0xff1f2838);
            text      = juce::Colour (0xffc9d6e6);
            dimText   = juce::Colour (0xff6b7a90);
            ice       = juce::Colour (0xff6fe3ff);
            iceDeep   = juce::Colour (0xff2b8cff);
            iceGlow   = juce::Colour (0xffb7f4ff);
            knobBody  = juce::Colour (0xff1a2130);
            knobRing  = juce::Colour (0xff262f42);
            screen    = juce::Colour (0xff070a10);
        }
    }
}

class IceLookAndFeel : public juce::LookAndFeel_V4
{
public:
    IceLookAndFeel() { refreshColours(); }

    void refreshColours()
    {
        using namespace IceColours;
        setColour (juce::ResizableWindow::backgroundColourId, bg);
        setColour (juce::Slider::textBoxTextColourId, text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Label::textColourId, text);
        setColour (juce::ComboBox::backgroundColourId, knobBody);
        setColour (juce::ComboBox::outlineColourId, panelEdge);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::arrowColourId, ice);
        setColour (juce::PopupMenu::backgroundColourId, panel);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, iceDeep.withAlpha (0.5f));
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::TextButton::buttonColourId, knobBody);
        setColour (juce::TextButton::buttonOnColourId, iceDeep);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        setColour (juce::ToggleButton::textColourId, text);
        setColour (juce::ToggleButton::tickColourId, ice);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& slider) override
    {
        using namespace IceColours;
        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.f);
        const float size = juce::jmin (bounds.getWidth(), bounds.getHeight());
        bounds = bounds.withSizeKeepingCentre (size, size);
        const float radius = size * 0.5f;
        const float cx = bounds.getCentreX(), cy = bounds.getCentreY();
        const float angle = startAngle + pos * (endAngle - startAngle);
        const float lineW = juce::jmax (2.5f, radius * 0.14f);
        const float arcR = radius - lineW * 0.5f;

        // background ring
        juce::Path bgArc;
        bgArc.addCentredArc (cx, cy, arcR, arcR, 0.f, startAngle, endAngle, true);
        g.setColour (knobRing);
        g.strokePath (bgArc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // value arc (bipolar if range crosses zero)
        const bool bipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const float zeroPos = bipolar ? (float) slider.valueToProportionOfLength (0.0) : 0.f;
        const float a0 = startAngle + zeroPos * (endAngle - startAngle);
        juce::Path valArc;
        valArc.addCentredArc (cx, cy, arcR, arcR, 0.f, juce::jmin (a0, angle), juce::jmax (a0, angle), true);
        juce::ColourGradient grad (iceDeep, cx - radius, cy + radius, iceGlow, cx + radius, cy - radius, false);
        g.setGradientFill (grad);
        g.strokePath (valArc, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // glow
        if (slider.isMouseOverOrDragging())
        {
            g.setColour (ice.withAlpha (0.18f));
            g.strokePath (valArc, juce::PathStrokeType (lineW * 2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // body
        const float bodyR = radius - lineW * 1.9f;
        juce::ColourGradient body (knobBody.brighter (0.25f), cx, cy - bodyR, knobBody.darker (0.4f), cx, cy + bodyR, false);
        g.setGradientFill (body);
        g.fillEllipse (cx - bodyR, cy - bodyR, bodyR * 2.f, bodyR * 2.f);
        g.setColour (panelEdge);
        g.drawEllipse (cx - bodyR, cy - bodyR, bodyR * 2.f, bodyR * 2.f, 1.f);

        // pointer
        juce::Path p;
        p.addRoundedRectangle (-lineW * 0.35f, -bodyR + 3.f, lineW * 0.7f, bodyR * 0.45f, lineW * 0.3f);
        g.setColour (iceGlow);
        g.fillPath (p, juce::AffineTransform::rotation (angle).translated (cx, cy));
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
    {
        using namespace IceColours;
        auto r = juce::Rectangle<float> (0.f, 0.f, (float) w, (float) h).reduced (0.5f);
        g.setColour (knobBody);
        g.fillRoundedRectangle (r, 5.f);
        g.setColour (box.hasKeyboardFocus (true) ? ice : panelEdge);
        g.drawRoundedRectangle (r, 5.f, 1.f);

        juce::Path arrow;
        const float ax = (float) w - 14.f, ay = (float) h * 0.5f;
        arrow.addTriangle (ax - 4.f, ay - 2.f, ax + 4.f, ay - 2.f, ax, ay + 3.f);
        g.setColour (ice);
        g.fillPath (arrow);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::Font (juce::FontOptions (13.f)); }
    juce::Font getLabelFont (juce::Label& l) override { return l.getFont(); }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        using namespace IceColours;
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = b.getToggleState() ? iceDeep : knobBody;
        if (over) c = c.brighter (0.15f);
        if (down) c = c.darker (0.2f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 5.f);
        g.setColour (b.getToggleState() ? ice : panelEdge);
        g.drawRoundedRectangle (r, 5.f, 1.f);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool) override
    {
        using namespace IceColours;
        auto r = b.getLocalBounds().toFloat();
        auto led = r.removeFromLeft (r.getHeight()).reduced (4.f);
        g.setColour (b.getToggleState() ? ice : knobRing);
        g.fillEllipse (led);
        if (b.getToggleState())
        {
            g.setColour (ice.withAlpha (0.35f));
            g.fillEllipse (led.expanded (3.f));
        }
        g.setColour (over ? text.brighter() : text);
        g.setFont (juce::Font (juce::FontOptions (12.f, juce::Font::bold)));
        g.drawText (b.getButtonText(), r.toNearestInt(), juce::Justification::centredLeft);
    }

    // Draws a toggle button as a flat "tab" with an underline when active
    void drawTabButton (juce::Graphics& g, juce::Button& b, bool over)
    {
        using namespace IceColours;
        auto r = b.getLocalBounds().toFloat();
        const bool on = b.getToggleState();
        g.setColour (on ? ice.withAlpha (0.14f) : (over ? knobBody.brighter (0.1f) : knobBody));
        g.fillRoundedRectangle (r.reduced (0.5f), 5.f);
        g.setColour (on ? ice : panelEdge);
        g.drawRoundedRectangle (r.reduced (0.5f), 5.f, 1.f);
        if (on) { g.setColour (ice); g.fillRoundedRectangle (r.getX() + 8.f, r.getBottom() - 4.f, r.getWidth() - 16.f, 2.f, 1.f); }
        g.setColour (on ? iceGlow : text);
        g.setFont (juce::Font (juce::FontOptions (11.f, juce::Font::bold)));
        g.drawText (b.getButtonText(), r.toNearestInt(), juce::Justification::centred);
    }

    void drawPopupMenuBackground (juce::Graphics& g, int w, int h) override
    {
        g.setColour (IceColours::panel);
        g.fillRect (0, 0, w, h);
        g.setColour (IceColours::panelEdge);
        g.drawRect (0, 0, w, h);
    }
};
