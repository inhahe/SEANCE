#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../Controller.h"

namespace PresetRecorder {

// SoundShop2's skip list as a tristate checkbox tree:
//
//   [~] All skip-listed plugins (3)
//       [ ] u-he (3)
//           [ ] Podolski        Podolski.vst3
//           [x] TyrellN6        TyrellN6.vst3
//           ...
//
// A tick means "record it anyway" (unskip). Ticking the top row unskips
// everything, ticking a company unskips all of that company's entries, and
// individual rows can be ticked one by one - or select several rows (Ctrl/
// Shift-click) and use "Unskip selected". A group's box shows a dash when only
// some of its entries are ticked.
//
// Unskipping is remembered by this tool only; SoundShop2's own file is never
// changed, so SoundShop2 keeps skipping them.
class SkipListTab : public juce::Component,
                    private juce::ChangeListener,
                    private juce::KeyListener
{
public:
    explicit SkipListTab(Controller&);
    ~SkipListTab() override;

    void resized() override;
    void paint(juce::Graphics&) override;

    void rebuild();

    // Called by tree items.
    void setEntriesUnskipped(const juce::StringArray& keys, bool unskip);
    void selectionChanged();
    bool isUnskipped(const juce::String& key) const;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    bool keyPressed(const juce::KeyPress&, juce::Component*) override;
    juce::StringArray keysUnderSelection() const;
    void updateDetails();
    void updateButtons();

    Controller& controller;

    juce::Label title, explanation;
    juce::TreeView tree;
    std::unique_ptr<juce::TreeViewItem> root;
    juce::TextButton unskipAllButton { "Unskip all" }, skipAllButton { "Skip all" },
                     unskipSelectedButton { "Unskip selected" }, skipSelectedButton { "Skip selected" },
                     revealButton { "Show in Explorer" }, copyPathButton { "Copy path" };
    juce::TextEditor details;
    juce::String selectedKey;
    juce::StringArray lastKeys;
    juce::String lastSignature;
};

} // namespace PresetRecorder
