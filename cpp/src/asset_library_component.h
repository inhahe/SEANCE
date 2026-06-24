#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include "asset_library.h"

namespace SoundShop {

struct NodeGraph;  // for editing FrequencyGraph assets + propagating to links

// ============================================================================
// AssetLibraryComponent - the management UI for the project-level asset library
// ("stores"). Opened from Edit -> Asset Library... It presents one tab per
// AssetKind (Waveforms, Instruments, ADHSR Curves, Morph Algorithms), each
// listing the user-published entries in that store with actions to Rename,
// Duplicate, Star, and Archive/Restore (soft-delete).
//
// There is deliberately NO hard-delete button: assets can be referenced by id
// from node scripts (waveforms, morph algorithms, frequency graphs), so an
// in-UI purge would silently dangle those references. Removal from the UI is
// archive-only (hides from pickers, stays resolvable). A true hard purge is
// reserved for an explicit, warned CLI action - the model still exposes
// AssetLibrary::erase() for that path, it just isn't wired to a button here.
//
// The list can be filtered by two toggles: "Show archived" (include soft-
// deleted entries) and "Starred only" (show just the user's favourites).
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
    // The full graph is taken (not just its AssetLibrary) so editing a curve
    // asset in place can re-resolve every node that links to it.
    AssetLibraryComponent(NodeGraph& graph,
                          std::function<void(const std::string&)> onEdit);
    ~AssetLibraryComponent() override;

    void resized() override;

private:
    class StorePanel;
    void doImport();
    void doExport();
    void refreshAllPanels();

    NodeGraph& graph;
    AssetLibrary& lib;
    std::function<void(const std::string&)> onEdit;
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    juce::TextButton importBtn { juce::String::fromUTF8("Import\xe2\x80\xa6") };
    juce::TextButton exportBtn { juce::String::fromUTF8("Export\xe2\x80\xa6") };
    std::vector<StorePanel*> panels;             // for refresh-after-import
    std::unique_ptr<juce::FileChooser> chooser;  // kept alive across async pick

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AssetLibraryComponent)
};

} // namespace SoundShop
