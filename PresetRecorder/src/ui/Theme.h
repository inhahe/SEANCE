#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// Colours and small layout helpers shared by the tabs. The base look is
// SEANCE's AppLookAndFeel (JUCE's dark LookAndFeel_V4), installed in Main.cpp.
namespace PresetRecorder::Theme {

inline const juce::Colour background   { 0xff26282c };
inline const juce::Colour panel        { 0xff2f3237 };
inline const juce::Colour rowSelected  { 0xff34506e };
inline const juce::Colour rowAlt       { 0xff2b2e33 };
inline const juce::Colour text         { 0xffe8e8e8 };
inline const juce::Colour dimText      { 0xff9a9ea6 };
inline const juce::Colour good         { 0xff7fd28a };
inline const juce::Colour warn         { 0xffe6c65c };
inline const juce::Colour bad          { 0xffe57373 };
inline const juce::Colour accent       { 0xff5aa9e6 };
inline const juce::Colour skipped      { 0xffb08a5a };

inline juce::Font font(float height, bool bold = false)
{
    return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
}

// Status text -> colour, for the "ok/silent/failed/..." vocabulary used everywhere.
inline juce::Colour forStatus(const juce::String& s)
{
    if (s.startsWith("not recorded") || s.startsWith("pending"))
        return dimText;
    if (s.startsWith("ok") || s.startsWith("done") || s.containsIgnoreCase("recorded"))
        return good;
    if (s.startsWith("silent") || s.startsWith("partial") || s.startsWith("timeout") || s.startsWith("stopped"))
        return warn;
    if (s.startsWith("fail") || s.startsWith("crash") || s.startsWith("scan failed") || s.startsWith("no effect"))
        return bad;
    if (s.startsWith("skipped"))
        return skipped;
    if (s.startsWith("recording") || s.startsWith("loading") || s.startsWith("starting") || s.startsWith("scanning")
        || s.startsWith("restarting"))
        return accent;
    return dimText;
}

// A label sized for a form row.
inline void styleFormLabel(juce::Label& l, const juce::String& labelText)
{
    l.setText(labelText, juce::dontSendNotification);
    l.setJustificationType(juce::Justification::centredRight);
    l.setColour(juce::Label::textColourId, dimText);
    l.setFont(font(13.0f));
}

} // namespace PresetRecorder::Theme
