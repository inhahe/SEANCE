#include "asset_library_component.h"

namespace SoundShop {

// ---------------------------------------------------------------------------
// StorePanel - one tab: a list of one AssetKind's entries plus action buttons.
// ---------------------------------------------------------------------------
class AssetLibraryComponent::StorePanel : public juce::Component,
                                          public juce::ListBoxModel {
public:
    StorePanel(AssetLibrary& lib, AssetKind kind,
               std::function<void(const std::string&)> onEdit)
        : lib(lib), kind(kind), onEdit(std::move(onEdit)) {
        list.setModel(this);
        list.setRowHeight(24);
        list.setColour(juce::ListBox::backgroundColourId, juce::Colour(30, 30, 34));
        addAndMakeVisible(list);

        addAndMakeVisible(showArchived);
        showArchived.setButtonText("Show archived");
        showArchived.onClick = [this] { refresh(); };

        auto setup = [this](juce::TextButton& b, const juce::String& t) {
            b.setButtonText(t);
            addAndMakeVisible(b);
            b.onClick = [this, &b] { onButton(&b); };
        };
        setup(renameBtn,    "Rename");
        setup(duplicateBtn, "Duplicate");
        setup(starBtn,      "Star");
        setup(archiveBtn,   "Archive");
        setup(deleteBtn,    "Delete");

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
        archiveBtn.setTooltip("Soft-delete: hide the asset from pickers while keeping "
                              "it resolvable so existing references stay valid. "
                              "Toggles to Restore for an archived asset.");
        deleteBtn.setTooltip("Permanently remove the asset. Any references to it will "
                             "dangle. Cannot be undone via this list - use sparingly.");
        refresh();
    }

    void resized() override {
        auto r = getLocalBounds().reduced(8);
        auto bottom = r.removeFromBottom(34);
        r.removeFromBottom(6);
        list.setBounds(r);
        showArchived.setBounds(bottom.removeFromLeft(130).withSizeKeepingCentre(130, 24));
        bottom.removeFromLeft(8);
        const int bw = 84;
        renameBtn.setBounds(bottom.removeFromLeft(bw).reduced(2, 4));
        duplicateBtn.setBounds(bottom.removeFromLeft(bw).reduced(2, 4));
        starBtn.setBounds(bottom.removeFromLeft(bw).reduced(2, 4));
        archiveBtn.setBounds(bottom.removeFromLeft(bw).reduced(2, 4));
        deleteBtn.setBounds(bottom.removeFromLeft(bw).reduced(2, 4));
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
        for (const AssetEntry* e : lib.list(kind, showArchived.getToggleState()))
            rows.push_back(e->id);
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
        renameBtn.setEnabled(has);
        duplicateBtn.setEnabled(has);
        starBtn.setEnabled(has);
        archiveBtn.setEnabled(has);
        deleteBtn.setEnabled(has);
        archiveBtn.setButtonText(has && e->archived ? "Restore" : "Archive");
        starBtn.setButtonText(has && e->starred ? "Unstar" : "Star");
    }

    void onButton(juce::TextButton* b) {
        if (b == &renameBtn)    doRename();
        else if (b == &duplicateBtn) doDuplicate();
        else if (b == &starBtn)      doStarToggle();
        else if (b == &archiveBtn)   doArchiveToggle();
        else if (b == &deleteBtn)    doDelete();
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

    void doDelete() {
        const AssetEntry* e = lib.find(selectedId());
        if (!e) return;
        int id = e->id;
        juce::String nm = juce::String(e->name.empty() ? "(unnamed)" : e->name);
        juce::NativeMessageBox::showYesNoBox(
            juce::MessageBoxIconType::WarningIcon,
            "Delete asset permanently?",
            "Delete \"" + nm + "\" (#" + juce::String(id) + ") from the library?\n\n"
            "This cannot be undone from this list. Any node or editor still "
            "referencing this asset by id will lose it (the reference will dangle). "
            "If you only want to hide it, use Archive instead.",
            this,
            juce::ModalCallbackFunction::create([this, id](int res) {
                if (res == 1 && lib.erase(id)) {
                    onEdit("Delete asset");
                    refresh();
                }
            }));
    }

    void selectId(int id) {
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i] == id) { list.selectRow((int) i); return; }
    }

    AssetLibrary& lib;
    AssetKind kind;
    std::function<void(const std::string&)> onEdit;
    juce::ListBox list;
    juce::ToggleButton showArchived;
    juce::TextButton renameBtn, duplicateBtn, starBtn, archiveBtn, deleteBtn;
    std::vector<int> rows;   // asset ids currently displayed (stable, not Node*)
};

// ---------------------------------------------------------------------------
// AssetLibraryComponent
// ---------------------------------------------------------------------------
AssetLibraryComponent::AssetLibraryComponent(
        AssetLibrary& library, std::function<void(const std::string&)> onEditCb)
    : lib(library), onEdit(std::move(onEditCb)) {
    auto bg = juce::Colour(40, 40, 45);
    auto add = [&](const juce::String& title, AssetKind kind) {
        tabs.addTab(title, bg, new StorePanel(lib, kind, onEdit), true);
    };
    add("Waveforms",   AssetKind::Waveform);
    add("Instruments", AssetKind::Instrument);
    add("ADHSR Curves", AssetKind::AhdsrCurve);
    add("Morph Algorithms", AssetKind::MorphAlgorithm);
    addAndMakeVisible(tabs);
    setSize(680, 460);
}

AssetLibraryComponent::~AssetLibraryComponent() = default;

void AssetLibraryComponent::resized() {
    tabs.setBounds(getLocalBounds());
}

} // namespace SoundShop
