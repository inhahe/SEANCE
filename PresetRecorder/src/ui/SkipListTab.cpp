#include "SkipListTab.h"
#include "Theme.h"

namespace PresetRecorder {

//==============================================================================
namespace
{
    constexpr int kCheckboxWidth = 24;

    class CheckItem : public juce::TreeViewItem
    {
    public:
        enum class State { off, on, mixed };

        explicit CheckItem(SkipListTab& o) : owner(o) {}

        virtual State getState() const = 0;
        virtual juce::StringArray getKeys() const = 0;
        virtual juce::String getLabel() const = 0;
        virtual juce::String getSubLabel() const { return {}; }
        virtual bool isGroup() const { return false; }

        int getItemHeight() const override { return 24; }

        void paintItem(juce::Graphics& g, int width, int height) override
        {
            if (isSelected())
                g.fillAll(Theme::rowSelected);

            // The tristate box.
            const auto box = juce::Rectangle<float>(5.0f, (float) (height - 14) * 0.5f, 14.0f, 14.0f);
            const auto state = getState();
            g.setColour(state == State::off ? Theme::panel.brighter(0.15f) : Theme::accent);
            g.fillRoundedRectangle(box, 2.5f);
            g.setColour(Theme::text.withAlpha(0.85f));
            g.drawRoundedRectangle(box, 2.5f, 1.2f);

            if (state == State::on)
            {
                juce::Path tick;
                tick.startNewSubPath(box.getX() + 3.0f, box.getCentreY());
                tick.lineTo(box.getX() + 6.0f, box.getBottom() - 3.5f);
                tick.lineTo(box.getRight() - 3.0f, box.getY() + 3.5f);
                g.setColour(juce::Colours::white);
                g.strokePath(tick, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            else if (state == State::mixed)
            {
                g.setColour(juce::Colours::white);
                g.fillRect(box.reduced(3.5f, 6.0f));
            }

            // Label, and a dimmer sub-label on the right.
            auto textArea = juce::Rectangle<int>(kCheckboxWidth + 4, 0, width - kCheckboxWidth - 8, height);
            const auto sub = getSubLabel();
            if (sub.isNotEmpty())
            {
                g.setColour(Theme::dimText);
                g.setFont(Theme::font(12.0f));
                const int subWidth = juce::jmin(textArea.getWidth() / 2, 420);
                g.drawFittedText(sub, textArea.removeFromRight(subWidth), juce::Justification::centredRight, 1);
            }

            g.setColour(Theme::text);
            g.setFont(Theme::font(13.5f, isGroup()));
            g.drawFittedText(getLabel(), textArea, juce::Justification::centredLeft, 1);
        }

        void itemClicked(const juce::MouseEvent& e) override
        {
            if (e.x < kCheckboxWidth)
                toggle();
        }

        void itemDoubleClicked(const juce::MouseEvent& e) override
        {
            if (e.x < kCheckboxWidth)
                return; // the first click already toggled it
            if (isGroup())
                setOpen(! isOpen());
            else
                toggle();
        }

        void itemSelectionChanged(bool) override { owner.selectionChanged(); }

        void toggle() { owner.setEntriesUnskipped(getKeys(), getState() != State::on); }

    protected:
        SkipListTab& owner;
    };

    class EntryItem final : public CheckItem
    {
    public:
        EntryItem(SkipListTab& o, juce::String k, juce::String name, juce::String file, juce::String tip)
            : CheckItem(o), key(std::move(k)), label(std::move(name)), fileLabel(std::move(file)), tooltip(std::move(tip)) {}

        State getState() const override               { return owner.isUnskipped(key) ? State::on : State::off; }
        juce::StringArray getKeys() const override     { return { key }; }
        juce::String getLabel() const override         { return label; }
        juce::String getSubLabel() const override      { return fileLabel; }
        bool mightContainSubItems() override           { return false; }
        juce::String getUniqueName() const override    { return "entry:" + key; }
        juce::String getTooltip() override             { return tooltip; }

        const juce::String key;

    private:
        juce::String label, fileLabel, tooltip;
    };

    class GroupItem final : public CheckItem
    {
    public:
        GroupItem(SkipListTab& o, juce::String name, juce::String unique)
            : CheckItem(o), label(std::move(name)), uniqueName(std::move(unique)) {}

        State getState() const override
        {
            int on = 0, total = 0;
            for (auto& k : getKeys())
            {
                ++total;
                on += owner.isUnskipped(k) ? 1 : 0;
            }
            return on == 0 ? State::off : (on == total ? State::on : State::mixed);
        }

        juce::StringArray getKeys() const override
        {
            juce::StringArray keys;
            for (int i = 0; i < getNumSubItems(); ++i)
                if (auto* c = dynamic_cast<CheckItem*>(getSubItem(i)))
                    keys.addArray(c->getKeys());
            return keys;
        }

        juce::String getLabel() const override
        {
            const auto keys = getKeys();
            int on = 0;
            for (auto& k : keys)
                on += owner.isUnskipped(k) ? 1 : 0;
            return label + "  (" + juce::String(keys.size()) + (on > 0 ? ", " + juce::String(on) + " unskipped" : juce::String()) + ")";
        }

        bool isGroup() const override               { return true; }
        bool mightContainSubItems() override        { return true; }
        juce::String getUniqueName() const override { return uniqueName; }
        juce::String getTooltip() override
        {
            return "Tick to record everything in this group anyway; untick to skip it all again.";
        }

    private:
        juce::String label, uniqueName;
    };
}

//==============================================================================
SkipListTab::SkipListTab(Controller& c) : controller(c)
{
    title.setText("SoundShop2's skip list", juce::dontSendNotification);
    title.setFont(Theme::font(17.0f, true));
    addAndMakeVisible(title);

    explanation.setText("These are the plugins in the [Blocked] section of SoundShop2's soundshop_plugins.cfg. "
                        "SoundShop2 puts a plugin there when scanning it crashes or fails (and you can add plugins "
                        "by hand), and this tool skips them too - unless you tick them here. A tick means \"record "
                        "it anyway\". Your choice is remembered by this tool only: SoundShop2's file is never changed, "
                        "so SoundShop2 keeps skipping them. Changes apply from the next scan or recording run.",
                        juce::dontSendNotification);
    explanation.setFont(Theme::font(13.0f));
    explanation.setColour(juce::Label::textColourId, Theme::dimText);
    explanation.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(explanation);

    tree.setMultiSelectEnabled(true);
    tree.setDefaultOpenness(true);
    tree.setRootItemVisible(true);
    tree.setIndentSize(18);
    tree.setColour(juce::TreeView::backgroundColourId, Theme::panel);
    tree.addKeyListener(this);
    tree.setTitle("Skip list");
    addAndMakeVisible(tree);

    unskipAllButton.setTooltip("Record every plugin on SoundShop2's skip list anyway (in this tool only).");
    skipAllButton.setTooltip("Go back to skipping everything on SoundShop2's skip list.");
    unskipSelectedButton.setTooltip("Record the selected entries anyway. Selecting a company row selects all of its "
                                    "entries. Ctrl-click or Shift-click to select several rows. (Space does the same "
                                    "for the selection: ticks it, or unticks it if it's all ticked.)");
    skipSelectedButton.setTooltip("Skip the selected entries again.");
    revealButton.setTooltip("Show the selected plugin's file in Explorer.");
    copyPathButton.setTooltip("Copy the selected plugin's full path to the clipboard.");

    unskipAllButton.onClick = [this] { setEntriesUnskipped(lastKeys, true); };
    skipAllButton.onClick = [this] { setEntriesUnskipped(lastKeys, false); };
    unskipSelectedButton.onClick = [this] { setEntriesUnskipped(keysUnderSelection(), true); };
    skipSelectedButton.onClick = [this] { setEntriesUnskipped(keysUnderSelection(), false); };

    revealButton.onClick = [this]
    {
        if (auto* f = controller.getCatalog().findFile(selectedKey))
            if (juce::File::isAbsolutePath(f->fileOrId))
                juce::File(f->fileOrId).revealToUser();
    };

    copyPathButton.onClick = [this]
    {
        if (auto* f = controller.getCatalog().findFile(selectedKey))
            juce::SystemClipboard::copyTextToClipboard(f->fileOrId);
    };

    for (auto* b : { &unskipAllButton, &skipAllButton, &unskipSelectedButton, &skipSelectedButton,
                     &revealButton, &copyPathButton })
        addAndMakeVisible(*b);

    details.setMultiLine(true, true);
    details.setReadOnly(true);
    details.setCaretVisible(false);
    details.setScrollbarsShown(true);
    details.setFont(Theme::font(13.0f));
    details.setColour(juce::TextEditor::backgroundColourId, Theme::panel);
    details.setColour(juce::TextEditor::outlineColourId, Theme::panel);
    addAndMakeVisible(details);

    controller.addChangeListener(this);
    rebuild();
}

SkipListTab::~SkipListTab()
{
    controller.removeChangeListener(this);
    tree.removeKeyListener(this);
    tree.setRootItem(nullptr);
}

void SkipListTab::paint(juce::Graphics& g)
{
    g.fillAll(Theme::background);
}

void SkipListTab::resized()
{
    auto area = getLocalBounds().reduced(12);
    title.setBounds(area.removeFromTop(26));
    explanation.setBounds(area.removeFromTop(56));
    area.removeFromTop(6);

    auto buttons = area.removeFromBottom(30);
    for (auto* b : { &unskipAllButton, &skipAllButton, &unskipSelectedButton, &skipSelectedButton })
    {
        b->setBounds(buttons.removeFromLeft(130).reduced(0, 1));
        buttons.removeFromLeft(8);
    }
    area.removeFromBottom(8);

    auto right = area.removeFromRight(juce::jmax(320, area.getWidth() * 2 / 5));
    area.removeFromRight(10);
    tree.setBounds(area);

    auto detailButtons = right.removeFromBottom(30);
    revealButton.setBounds(detailButtons.removeFromLeft(140).reduced(0, 1));
    detailButtons.removeFromLeft(8);
    copyPathButton.setBounds(detailButtons.removeFromLeft(100).reduced(0, 1));
    right.removeFromBottom(8);
    details.setBounds(right);
}

bool SkipListTab::isUnskipped(const juce::String& key) const
{
    return controller.getCatalog().getUnskipped().contains(key);
}

void SkipListTab::setEntriesUnskipped(const juce::StringArray& keys, bool unskip)
{
    if (keys.isEmpty())
        return;

    auto current = controller.getSettings().getUnskipped();
    for (auto& k : keys)
    {
        if (unskip)
            current.addIfNotAlreadyThere(k);
        else
            current.removeString(k);
    }

    controller.setUnskipped(current);
    tree.repaint();
    updateDetails();
    updateButtons();
}

juce::StringArray SkipListTab::keysUnderSelection() const
{
    juce::StringArray keys;
    for (int i = 0; i < tree.getNumSelectedItems(); ++i)
        if (auto* item = dynamic_cast<CheckItem*>(tree.getSelectedItem(i)))
            for (auto& k : item->getKeys())
                keys.addIfNotAlreadyThere(k);
    return keys;
}

bool SkipListTab::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    if (key == juce::KeyPress::spaceKey)
    {
        const auto keys = keysUnderSelection();
        bool allOn = ! keys.isEmpty();
        for (auto& k : keys)
            allOn = allOn && isUnskipped(k);
        setEntriesUnskipped(keys, ! allOn);
        return true;
    }
    return false;
}

void SkipListTab::changeListenerCallback(juce::ChangeBroadcaster*)
{
    rebuild();
}

void SkipListTab::rebuild()
{
    auto& catalog = controller.getCatalog();
    auto entries = catalog.getSkipListEntries();

    struct Row { juce::String key, company, name, file, tip; };
    std::vector<Row> rows;
    juce::StringArray keys;
    juce::String signature;

    for (auto* f : entries)
    {
        const auto d = catalog.describeSkipEntry(*f);
        Row r;
        r.key = f->key;
        r.company = d.company;
        r.name = d.displayName;
        r.file = juce::File::isAbsolutePath(f->fileOrId) ? juce::File(f->fileOrId).getFileName() : f->fileOrId;
        if (f->fileInfo.architecture.isNotEmpty())
            r.file << "  -  " << f->fileInfo.architecture;
        if (! f->fileInfo.exists)
            r.file << "  -  missing";
        r.tip = f->fileOrId;
        rows.push_back(r);
        keys.add(r.key);
        signature << r.key << "|" << r.company << "|" << r.name << "|" << r.file << "\n";
    }

    lastKeys = keys;

    // Only rebuild the tree when its content changed (not on every tick of a
    // recording run), so openness and selection survive.
    if (root != nullptr && signature == lastSignature)
    {
        tree.repaint();
        updateDetails();
        updateButtons();
        return;
    }
    lastSignature = signature;

    std::unique_ptr<juce::XmlElement> openness;
    if (root != nullptr)
        openness = tree.getOpennessState(true);

    tree.setRootItem(nullptr);
    auto newRoot = std::make_unique<GroupItem>(*this, "All skip-listed plugins", "root");

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b)
    {
        auto c = a.company.compareIgnoreCase(b.company);
        return c != 0 ? c < 0 : a.name.compareIgnoreCase(b.name) < 0;
    });

    GroupItem* currentGroup = nullptr;
    juce::String currentCompany;
    for (auto& r : rows)
    {
        if (currentGroup == nullptr || ! r.company.equalsIgnoreCase(currentCompany))
        {
            currentCompany = r.company;
            currentGroup = new GroupItem(*this, r.company, "company:" + r.company.toLowerCase());
            newRoot->addSubItem(currentGroup);
        }
        currentGroup->addSubItem(new EntryItem(*this, r.key, r.name, r.file, r.tip));
    }

    root = std::move(newRoot);
    tree.setRootItem(root.get());
    if (openness != nullptr)
        tree.restoreOpennessState(*openness, true);

    updateDetails();
    updateButtons();
}

void SkipListTab::selectionChanged()
{
    selectedKey = {};
    if (tree.getNumSelectedItems() == 1)
        if (auto* entry = dynamic_cast<EntryItem*>(tree.getSelectedItem(0)))
            selectedKey = entry->key;

    updateDetails();
    updateButtons();
}

void SkipListTab::updateButtons()
{
    const bool any = ! lastKeys.isEmpty();
    const bool hasSelection = tree.getNumSelectedItems() > 0;
    const bool single = selectedKey.isNotEmpty();

    unskipAllButton.setEnabled(any);
    skipAllButton.setEnabled(any);
    unskipSelectedButton.setEnabled(hasSelection);
    skipSelectedButton.setEnabled(hasSelection);
    revealButton.setEnabled(single && juce::File::isAbsolutePath(selectedKey));
    copyPathButton.setEnabled(single);

    const juce::String needSelection = " (Disabled: select one or more rows in the list first.)";
    unskipSelectedButton.setTooltip("Record the selected entries anyway. Selecting a company row covers all of its "
                                    "entries; Ctrl/Shift-click selects several rows. Space does the same."
                                    + (hasSelection ? juce::String() : needSelection));
    skipSelectedButton.setTooltip("Skip the selected entries again." + (hasSelection ? juce::String() : needSelection));
    revealButton.setTooltip(single ? "Show the selected plugin's file in Explorer."
                                   : "Show the selected plugin's file in Explorer. (Disabled: select a single plugin row.)");
    copyPathButton.setTooltip(single ? "Copy the selected plugin's full path to the clipboard."
                                     : "Copy the selected plugin's full path. (Disabled: select a single plugin row.)");
}

void SkipListTab::updateDetails()
{
    auto& catalog = controller.getCatalog();

    if (lastKeys.isEmpty())
    {
        details.setText("SoundShop2's skip list is empty"
                        + juce::String(catalog.getSeanceConfig().configFound
                                           ? juce::String(".")
                                           : " (no soundshop_plugins.cfg was found - see the Record tab)."));
        return;
    }

    if (selectedKey.isEmpty())
    {
        const auto keys = keysUnderSelection();
        int on = 0;
        for (auto& k : keys)
            on += isUnskipped(k) ? 1 : 0;

        details.setText(keys.isEmpty()
                            ? "Select an entry to see where it is and why it's probably on the list."
                            : juce::String(keys.size()) + " entries selected, " + juce::String(on) + " of them unskipped.");
        return;
    }

    auto* f = catalog.findFile(selectedKey);
    if (f == nullptr)
    {
        details.setText({});
        return;
    }

    const auto d = catalog.describeSkipEntry(*f);
    const auto& info = f->fileInfo;
    juce::String t;

    t << d.displayName << "\n\n";
    t << "Path on disk:\n" << f->fileOrId << "\n\n";
    t << "Format: " << f->format;
    if (info.architecture.isNotEmpty())
        t << "    Architecture: " << info.architecture;
    t << "\n";
    if (info.isFile)
        t << "File exists: " << (info.exists ? "yes" : "NO") << "\n";
    t << "Company: " << d.company << "\n   (from " << d.companySource << ")\n";
    if (! d.names.isEmpty())
        t << "Plugins inside: " << d.names.joinIntoString(", ") << "\n";
    if (info.version.isNotEmpty())
        t << "Version: " << info.version << "\n";
    t << "\n";

    if (! isUnskipped(f->key))
        t << "Status: skipped.\n";
    else if (f->state == PluginFile::State::scanned)
        t << "Status: unskipped and scanned - it will be recorded on the next run.\n";
    else if (f->state == PluginFile::State::scanFailed)
        t << "Status: unskipped, but scanning it failed: " << f->error << "\n";
    else
        t << "Status: unskipped - it will be scanned (in a separate process) and recorded on the next run.\n";

    t << "\nWhy it's probably on the list:\n";
    if (info.isFile && ! info.exists)
        t << "The file doesn't exist any more (moved or uninstalled), so there's nothing to record.";
    else if (! info.loadableArchitecture)
        t << "It is a " << info.architecture << " plugin. SoundShop2 and this tool are 64-bit programs and "
             "can't load it at all, so SoundShop2's scan failed and put it on the list automatically. "
             "Unskipping it will only produce another scan failure. Use its 64-bit version if you have it.";
    else
        t << "SoundShop2 adds a plugin to this list automatically when scanning it fails or crashes, and you "
             "can also block plugins by hand in its Plugin Settings. Recording it here is safe to try: this "
             "tool loads every plugin in a separate worker process, so if it crashes only that plugin is lost.";

    details.setText(t);
}

} // namespace PresetRecorder
