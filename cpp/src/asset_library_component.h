#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "asset_library.h"

namespace SoundShop {

// ============================================================================
// AssetLibraryComponent - the management UI for the project-level asset library
// ("stores"). Opened from Edit -> Asset Library... It presents one tab per
// AssetKind (Waveforms, Instruments, ADHSR Curves, Morph Algorithms), each
// listing the user-published entries in that store with actions to Rename,
// Duplicate, Archive/Restore (soft-delete), and Delete (hard purge, warned).
//
// It edits the live AssetLibrary in place. Every mutation is reported through
// the `onEdit` callback so the host (MainContentComponent) can commitSnapshot()
// + mark the project dirty - keeping store edits on the undo tree like any other
// project change.
//
// NOTE on user-vs-builtin: the AssetLibrary holds only USER-published assets;
// factory waveforms / warp methods live in their own banks (WaveformBank, the
// warp registry) and are immutable, so they are not shown here. Built-ins are
// browsable from the per-editor pickers (where you actually choose an asset),
// not from this management dialog, which exists to curate the editable store.
// ============================================================================
class AssetLibraryComponent : public juce::Component {
public:
    // onEdit(description) is invoked after any mutation so the host can commit an
    // undo snapshot and set the dirty flag. Description is a short verb phrase.
    AssetLibraryComponent(AssetLibrary& library,
                          std::function<void(const std::string&)> onEdit);
    ~AssetLibraryComponent() override;

    void resized() override;

private:
    class StorePanel;
    AssetLibrary& lib;
    std::function<void(const std::string&)> onEdit;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AssetLibraryComponent)
};

} // namespace SoundShop
