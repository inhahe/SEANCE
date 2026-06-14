#include "asset_library_component.h"
#include "asset_import.h"
#include "project_file.h"
#include "node_graph.h"
#include "curve_editor.h"     // SpectralCurve(Panel), resolveCurveEqReferences
#include "spectral_editor.h"  // resolveSpectralReferences
#include "spectrum_tap.h"     // resolveSpectrumTapReferences
#include "dialog_helpers.h"   // launchToolDialog
#include <fstream>

namespace SoundShop {

// ---------------------------------------------------------------------------
// FrequencyGraphAssetEditor - the library-side editor for a single
// FrequencyGraph (SpectralCurve) asset. Hosts a SpectralCurvePanel bound to a
// local copy of the asset's curve; every edit writes the curve back into the
// library entry (lib.update) and re-runs all three FrequencyGraph resolvers so
// any LIVE-LINKED consumer (Curve EQ, Spectral FFT mag/phase, Spectrum Tap)
// updates in place. This is the one sanctioned action-at-a-distance in the new
// fork-by-default / read-only-link model: a linked consumer is read-only, and
// the only way to change the shared curve is to edit it here in the library.
//
// A single undo snapshot is committed (via onDone) when the editor closes, iff
// something actually changed - the live lib.update/resolve calls during editing
// keep the audio graph current, but we don't want one undo step per drag tick.
// ---------------------------------------------------------------------------
class FrequencyGraphAssetEditor : public juce::Component {
public:
    FrequencyGraphAssetEditor(NodeGraph& graph, int assetId,
                              std::function<void()> onDone)
        : graph(graph), assetId(assetId), onDone(std::move(onDone)) {
        if (const AssetEntry* e = graph.assets.find(assetId))
            SpectralCurve::decode(e->payload, curve);
        curve.rebake();

        // Y-range: a FrequencyGraph asset can hold a phase curve (signed,
        // [-pi, pi]), a magnitude curve (0..1) or a gain curve (0..2). Detect a
        // signed curve and use the phase range; otherwise the [0, 2] gain range
        // comfortably contains both magnitude and gain shapes.
        float yMin = 0.0f, yMax = 2.0f;
        for (float v : curve.evaluate(256))
            if (v < -0.0001f) { yMin = -3.14159265f; yMax = 3.14159265f; break; }

        panel = std::make_unique<SpectralCurvePanel>(
            curve, "Curve", yMin, yMax, juce::Colour(150, 230, 170),
            [this]() { commit(); });
        addAndMakeVisible(panel.get());
        setSize(520, 360);
    }

    ~FrequencyGraphAssetEditor() override {
        if (dirty && onDone) onDone();
    }

    void resized() override { panel->setBounds(getLocalBounds().reduced(8)); }

private:
    void commit() {
        graph.assets.update(assetId, "", curve.encode());
        // Propagate to every live link in either direction's consumers.
        resolveCurveEqReferences(graph);
        resolveSpectralReferences(graph);
        resolveSpectrumTapReferences(graph);
        dirty = true;
    }

    NodeGraph& graph;
    int assetId;
    std::function<void()> onDone;
    SpectralCurve curve;
    std::unique_ptr<SpectralCurvePanel> panel;
    bool dirty = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FrequencyGraphAssetEditor)
};

// ---------------------------------------------------------------------------
// StorePanel - one tab: a list of one AssetKind's entries plus action buttons.
// ---------------------------------------------------------------------------
class AssetLibraryComponent::StorePanel : public juce::Component,
                                          public juce::ListBoxModel {
public:
    StorePanel(NodeGraph& graph, AssetKind kind,
               std::function<void(const std::string&)> onEdit)
        : graph(graph), lib(graph.assets), kind(kind),
          onEdit(std::move(onEdit)) {
        list.setModel(this);
        list.setRowHeight(24);
        list.setColour(juce::ListBox::backgroundColourId, juce::Colour(30, 30, 34));
        addAndMakeVisible(list);

        addAndMakeVisible(showArchived);
        showArchived.setButtonText("Show archived");
        showArchived.onClick = [this] { refresh(); };
        showArchived.setTooltip("Include soft-deleted (archived) assets in the list. "
                                "Archived assets stay resolvable so existing references "
                                "keep working; this just un-hides them here.");

        addAndMakeVisible(starredOnly);
        starredOnly.setButtonText("Starred only");
        starredOnly.onClick = [this] { refresh(); };
        starredOnly.setTooltip("Show only assets you've starred as favourites. Use the "
                               "Star button to mark the selected asset.");

        auto setup = [this](juce::TextButton& b, const juce::String& t) {
            b.setButtonText(t);
            addAndMakeVisible(b);
            b.onClick = [this, &b] { onButton(&b); };
        };
        // FrequencyGraph assets get an in-library curve editor (the only place
        // a live-linked, read-only consumer curve can actually be changed). The
        // other kinds have no library-side editor yet.
        if (kind == AssetKind::FrequencyGraph) {
            setup(editBtn, juce::String::fromUTF8("Edit\xe2\x80\xa6"));
            editBtn.setTooltip("Edit the stored frequency-graph curve. Changes "
                               "propagate live to every node that LINKS this "
                               "asset (linked curves are read-only in their own "
                               "editors - this is where you change the shared "
                               "curve). Forked copies are unaffected.");
        }
        setup(renameBtn,    "Rename");
        setup(duplicateBtn, "Duplicate");
        setup(starBtn,      "Star");
        setup(archiveBtn,   "Archive");

        starBtn.setTooltip("Mark the asset as a favourite (star). Pickers offer a "
                           "\"starred only\" filter so you can surface your favourites "
                           "out of a large library. Toggles to Unstar when already "
                           "starred.");

        renameBtn.setTooltip("Rename the selected asset. Names are display-only - "
                             "everything refers to the asset by id, so renaming "
                             "never breaks a reference.");
        duplicateBtn.setTooltip("Make an independent copy under a new id. Use this "
                                "to diverge: edit the copy while the original stays "
                                "shared by everything that referenced it.");
        archiveBtn.setTooltip("Remove the asset from pickers (soft-delete) while keeping "
                              "it resolvable so existing references stay valid. This is "
                              "the only removal action here - there is no hard delete, "
                              "because assets can be referenced by id from node scripts "
                              "and an in-UI purge would silently dangle them. Toggles to "
                              "Restore for an archived asset.");
        refresh();
    }

    void resized() override {
        auto r = getLocalBounds().reduced(8);
        // Two filter toggles stack vertically on the left; action buttons fill
        // the rest of the row to the right.
        auto bottom = r.removeFromBottom(52);
        r.removeFromBottom(6);
        list.setBounds(r);
        auto filters = bottom.removeFromLeft(120);
        showArchived.setBounds(filters.removeFromTop(24));
        filters.removeFromTop(4);
        starredOnly.setBounds(filters.removeFromTop(24));
        bottom.removeFromLeft(8);
        const int bw = 84;
        auto buttonRow = bottom.withSizeKeepingCentre(bottom.getWidth(), 32);
        if (editBtn.isVisible())
            editBtn.setBounds(buttonRow.removeFromLeft(bw).reduced(2, 4));
        renameBtn.setBounds(buttonRow.removeFromLeft(bw).reduced(2, 4));
        duplicateBtn.setBounds(buttonRow.removeFromLeft(bw).reduced(2, 4));
        starBtn.setBounds(buttonRow.removeFromLeft(bw).reduced(2, 4));
        archiveBtn.setBounds(buttonRow.removeFromLeft(bw).reduced(2, 4));
    }

    // ---- ListBoxModel ----
    int getNumRows() override { return (int) rows.size(); }

    void paintListBoxItem(int row, juce::Graphics& g, int w, int h,
                          bool selected) override {
        if (row < 0 || row >= (int) rows.size()) return;
        const AssetEntry* e = lib.find(rows[(size_t) row]);
        if (!e) return;
        if (selected) g.fillAll(juce::Colour(60, 90, 140));
        g.setColour(e->archived ? juce::Colours::grey : juce::Colours::white);
        juce::String label;
        if (e->starred) label << juce::String::fromUTF8("\xe2\x98\x85 "); // star prefix
        label << juce::String(e->name.empty() ? "(unnamed)" : e->name);
        if (e->archived) label += "   [archived]";
        g.setFont(15.0f);
        g.drawText(label, 10, 0, w - 70, h, juce::Justification::centredLeft);
        g.setColour(juce::Colours::grey);
        g.setFont(12.0f);
        g.drawText("#" + juce::String(e->id), w - 64, 0, 58, h,
                   juce::Justification::centredRight);
    }

    void selectedRowsChanged(int) override { updateButtons(); }
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override {
        if (row >= 0 && row < (int) rows.size()) doRename();
    }

    void refresh() {
        rows.clear();
        const bool starOnly = starredOnly.getToggleState();
        for (const AssetEntry* e : lib.list(kind, showArchived.getToggleState())) {
            if (starOnly && !e->starred) continue;
            rows.push_back(e->id);
        }
        list.updateContent();
        list.repaint();
        updateButtons();
    }

private:
    int selectedId() const {
        int r = list.getSelectedRow();
        if (r < 0 || r >= (int) rows.size()) return 0;
        return rows[(size_t) r];
    }

    void updateButtons() {
        const AssetEntry* e = lib.find(selectedId());
        bool has = e != nullptr;
        if (editBtn.isVisible()) editBtn.setEnabled(has && !e->archived);
        renameBtn.setEnabled(has);
        duplicateBtn.setEnabled(has);
        starBtn.setEnabled(has);
        archiveBtn.setEnabled(has);
        archiveBtn.setButtonText(has && e->archived ? "Restore" : "Archive");
        starBtn.setButtonText(has && e->starred ? "Unstar" : "Star");
    }

    void onButton(juce::TextButton* b) {
        if (b == &editBtn)      doEdit();
        else if (b == &renameBtn)    doRename();
        else if (b == &duplicateBtn) doDuplicate();
        else if (b == &starBtn)      doStarToggle();
        else if (b == &archiveBtn)   doArchiveToggle();
    }

    void doStarToggle() {
        const AssetEntry* e = lib.find(selectedId());
        if (!e) return;
        int id = e->id;
        bool wasStarred = e->starred;
        if (lib.setStarred(id, !wasStarred)) {
            onEdit(wasStarred ? "Unstar asset" : "Star asset");
            refresh();
        }
    }

    void doEdit() {
        const AssetEntry* e = lib.find(selectedId());
        if (!e || kind != AssetKind::FrequencyGraph) return;
        int id = e->id;
        juce::Component::SafePointer<StorePanel> safe(this);
        auto* editor = new FrequencyGraphAssetEditor(graph, id,
            [safe]() mutable {
                // Runs when the editor closes, iff it changed the curve. The
                // live lib.update/resolve already happened during editing; this
                // is the single undo snapshot + dirty flag for the whole edit.
                if (safe) { safe->onEdit("Edit frequency graph"); safe->refresh(); }
            });
        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(editor);
        opts.dialogTitle = "Edit Frequency Graph - " +
            juce::String(e->name.empty() ? "(unnamed)" : e->name);
        opts.dialogBackgroundColour = juce::Colour(40, 40, 45);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.resizable = true;
        opts.componentToCentreAround = this;
        launchToolDialog(opts);
    }

    void doRename() {
        const AssetEntry* e = lib.find(selectedId());
        if (!e) return;
        int id = e->id;
        auto* aw = new juce::AlertWindow("Rename asset",
            "Enter a new name (display-only):", juce::MessageBoxIconType::NoIcon, this);
        aw->addTextEditor("name", juce::String(e->name));
        aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        aw->enterModalState(true, juce::ModalCallbackFunction::create(
            [this, aw, id](int res) {
                if (res == 1) {
                    auto name = aw->getTextEditorContents("name").trim().toStdString();
                    if (!name.empty() && lib.rename(id, name)) {
                        onEdit("Rename asset");
                        refresh();
                    }
                }
                delete aw;
            }), true);
    }

    void doDuplicate() {
        const AssetEntry* e = lib.find(selectedId());
        if (!e) return;
        int id = e->id;
        juce::String dflt = juce::String(e->name) + " copy";
        auto* aw = new juce::AlertWindow("Duplicate asset",
            "Name for the copy:", juce::MessageBoxIconType::NoIcon, this);
        aw->addTextEditor("name", dflt);
        aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        aw->enterModalState(true, juce::ModalCallbackFunction::create(
            [this, aw, id](int res) {
                if (res == 1) {
                    auto name = aw->getTextEditorContents("name").trim().toStdString();
                    if (name.empty()) name = "copy";
                    int nid = lib.duplicate(id, name);
                    if (nid) {
                        onEdit("Duplicate asset");
                        refresh();
                        selectId(nid);
                    }
                }
                delete aw;
            }), true);
    }

    void doArchiveToggle() {
        const AssetEntry* e = lib.find(selectedId());
        if (!e) return;
        int id = e->id;
        bool wasArchived = e->archived;
        if (wasArchived ? lib.restore(id) : lib.archive(id)) {
            onEdit(wasArchived ? "Restore asset" : "Archive asset");
            refresh();
        }
    }

    void selectId(int id) {
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i] == id) { list.selectRow((int) i); return; }
    }

    NodeGraph& graph;        // full graph: editing a FrequencyGraph asset re-
                             // resolves every node that links to it
    AssetLibrary& lib;       // == graph.assets
    AssetKind kind;
    std::function<void(const std::string&)> onEdit;
    juce::ListBox list;
    juce::ToggleButton showArchived;
    juce::ToggleButton starredOnly;
    juce::TextButton editBtn;  // FrequencyGraph only (in-library curve editor)
    juce::TextButton renameBtn, duplicateBtn, starBtn, archiveBtn;
    std::vector<int> rows;   // asset ids currently displayed (stable, not Node*)
};

// ---------------------------------------------------------------------------
// AssetLibraryComponent
// ---------------------------------------------------------------------------
AssetLibraryComponent::AssetLibraryComponent(
        NodeGraph& graph, std::function<void(const std::string&)> onEditCb)
    : graph(graph), lib(graph.assets), onEdit(std::move(onEditCb)) {
    auto bg = juce::Colour(40, 40, 45);
    auto add = [&](const juce::String& title, AssetKind kind) {
        auto* p = new StorePanel(graph, kind, onEdit);
        panels.push_back(p);
        tabs.addTab(title, bg, p, true);
    };
    add("Waveforms",   AssetKind::Waveform);
    add("Instruments", AssetKind::Instrument);
    add("ADHSR Curves", AssetKind::AhdsrCurve);
    add("Morph Algorithms", AssetKind::MorphAlgorithm);
    add("Frequency Graphs", AssetKind::FrequencyGraph);
    addAndMakeVisible(tabs);

    addAndMakeVisible(importBtn);
    addAndMakeVisible(exportBtn);
    importBtn.onClick = [this] { doImport(); };
    exportBtn.onClick = [this] { doExport(); };
    importBtn.setTooltip("Merge assets from another project (.seance) or a library "
                         "export into this project's library. Identical assets are "
                         "deduplicated by content; new ids are assigned so nothing "
                         "collides; name clashes get a numeric suffix.");
    exportBtn.setTooltip("Write this project's entire asset library to a standalone "
                         "library file you can import into another project.");

    setSize(680, 460);
}

AssetLibraryComponent::~AssetLibraryComponent() = default;

void AssetLibraryComponent::resized() {
    auto r = getLocalBounds();
    auto bottom = r.removeFromBottom(38).reduced(8, 6);
    tabs.setBounds(r);
    exportBtn.setBounds(bottom.removeFromRight(96).reduced(2, 0));
    bottom.removeFromRight(6);
    importBtn.setBounds(bottom.removeFromRight(96).reduced(2, 0));
}

void AssetLibraryComponent::refreshAllPanels() {
    for (auto* p : panels) p->refresh();
}

void AssetLibraryComponent::doExport() {
    if (lib.size() == 0) {
        juce::NativeMessageBox::showMessageBoxAsync(
            juce::MessageBoxIconType::InfoIcon, "Nothing to export",
            "The asset library is empty - there is nothing to export yet.", this);
        return;
    }
    chooser = std::make_unique<juce::FileChooser>(
        "Export asset library", juce::File(), "*.seancelib;*.seance");
    chooser->launchAsync(
        juce::FileBrowserComponent::saveMode
            | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [this](const juce::FileChooser& fc) {
            juce::File f = fc.getResult();
            if (f == juce::File()) return;
            if (f.getFileExtension().isEmpty()) f = f.withFileExtension("seancelib");
            bool ok = ProjectFile::exportAssets(f.getFullPathName().toStdString(), lib);
            juce::NativeMessageBox::showMessageBoxAsync(
                ok ? juce::MessageBoxIconType::InfoIcon
                   : juce::MessageBoxIconType::WarningIcon,
                ok ? "Library exported" : "Export failed",
                ok ? ("Exported " + juce::String((int) lib.size())
                      + " asset(s) to:\n" + f.getFullPathName())
                   : ("Could not write:\n" + f.getFullPathName()),
                this);
        });
}

void AssetLibraryComponent::doImport() {
    chooser = std::make_unique<juce::FileChooser>(
        "Import asset library", juce::File(), "*.seancelib;*.seance");
    chooser->launchAsync(
        juce::FileBrowserComponent::openMode
            | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc) {
            juce::File f = fc.getResult();
            if (f == juce::File()) return;

            // Parse the source file into a throwaway graph via readProject (NOT
            // load(), which would clobber ProjectFile::currentPath). We only keep
            // its asset library; nodes/links in the temp graph are discarded.
            std::ifstream in(f.getFullPathName().toStdString());
            if (!in) {
                juce::NativeMessageBox::showMessageBoxAsync(
                    juce::MessageBoxIconType::WarningIcon, "Import failed",
                    "Could not open:\n" + f.getFullPathName(), this);
                return;
            }
            NodeGraph tmp;
            ProjectFile::readProject(in, tmp, nullptr);

            if (tmp.assets.size() == 0) {
                juce::NativeMessageBox::showMessageBoxAsync(
                    juce::MessageBoxIconType::InfoIcon, "No assets found",
                    "That file contains no asset-library entries to import.", this);
                return;
            }

            AssetImportResult res = importAssets(lib, tmp.assets.all());
            if (res.added > 0)
                onEdit("Import assets");   // snapshot + dirty only if something changed
            refreshAllPanels();

            juce::String msg;
            msg << "Added " << res.added << " new asset(s).\n"
                << res.deduped << " already existed (deduplicated by content).";
            if (res.renamed > 0)
                msg << "\n" << res.renamed << " renamed to avoid a name clash.";
            juce::NativeMessageBox::showMessageBoxAsync(
                juce::MessageBoxIconType::InfoIcon, "Import complete", msg, this);
        });
}

} // namespace SoundShop
