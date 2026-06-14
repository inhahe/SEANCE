#include "warp_editor.h"

#include <map>

namespace SoundShop {

namespace {
const char* domainLabel(WarpDomain d) {
    switch (d) {
        case WarpDomain::Phase:      return "Phase  -  remap read position";
        case WarpDomain::Amplitude:  return "Amplitude  -  shape the value";
        case WarpDomain::Modulation: return "Modulation";
        case WarpDomain::Spectral:   return "Spectral";
        case WarpDomain::Wavelet:    return "Wavelet";
        case WarpDomain::Granular:   return "Granular";
    }
    return "Other";
}

// "Soft Clip  *" for recommended methods; the star is the quality badge.
juce::String methodButtonText(WarpMethod m) {
    const auto* info = warpMethodInfo(m);
    if (!info) return "Pick warp...";
    juce::String t = info->name;
    if (info->recommended) t += "   " + juce::String(juce::CharPointer_UTF8("\xe2\x98\x85")); // U+2605 star
    return t;
}
} // namespace

WarpChainEditor::WarpChainEditor(Callbacks callbacks) : cb(std::move(callbacks)) {
    header.setText("Warp  (shape-bending)", juce::dontSendNotification);
    header.setTooltip("Shape-bending stages applied in order. Phase warps remap "
                      "the read position; amplitude warps reshape the value. "
                      "Methods marked with a star are the higher-quality picks.");
    header.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    addAndMakeVisible(header);

    addBtn.setButtonText("+ Add");
    addBtn.setTooltip("Add a shape-bending stage to this waveform.");
    addBtn.onClick = [this] { addOp(); };
    addAndMakeVisible(addBtn);
}

void WarpChainEditor::setChain(std::vector<WarpOp>* c) {
    chain = c;
    rebuild();
}

void WarpChainEditor::setAllowedDomains(std::vector<WarpDomain> domains,
                                        juce::String hint) {
    allowedDomains = std::move(domains);
    emptyHint = std::move(hint);
}

bool WarpChainEditor::domainAllowed(WarpDomain d) const {
    if (allowedDomains.empty()) return true;  // unrestricted
    for (WarpDomain a : allowedDomains) if (a == d) return true;
    return false;
}

int WarpChainEditor::preferredHeight() const {
    const int n = chain ? (int)chain->size() : 0;
    return kHeaderH + n * kRowH + 4;
}

void WarpChainEditor::addOp() {
    if (!chain) return;
    WarpOp op;
    // Default to the first recommended method (within the allowed domains, if
    // restricted) so a fresh stage does something audible immediately rather
    // than None / identity. Fall back to the first allowed method of any
    // quality, then to SoftClip.
    op.method = WarpMethod::SoftClip;
    bool picked = false;
    for (const auto& info : warpMethodRegistry()) {
        if (info.method == WarpMethod::None) continue;
        if (!domainAllowed(info.domain)) continue;
        if (info.recommended) { op.method = info.method; picked = true; break; }
    }
    if (!picked)
        for (const auto& info : warpMethodRegistry()) {
            if (info.method == WarpMethod::None) continue;
            if (!domainAllowed(info.domain)) continue;
            op.method = info.method; break;
        }
    op.amount = 0.5f;
    op.enabled = true;
    chain->push_back(op);
    rebuild();
    if (cb.onChanged) cb.onChanged();
    if (cb.onStructureChanged) cb.onStructureChanged();
}

void WarpChainEditor::removeOp(int idx) {
    if (!chain || idx < 0 || idx >= (int)chain->size()) return;
    chain->erase(chain->begin() + idx);
    rebuild();
    if (cb.onChanged) cb.onChanged();
    if (cb.onStructureChanged) cb.onStructureChanged();
}

void WarpChainEditor::moveOp(int idx, int delta) {
    if (!chain) return;
    const int j = idx + delta;
    if (idx < 0 || idx >= (int)chain->size()) return;
    if (j   < 0 || j   >= (int)chain->size()) return;
    std::swap((*chain)[idx], (*chain)[j]);
    rebuild();
    // onReorder first so positional bindings (a host's "Warp N" modulation
    // params) are fixed up BEFORE onChanged mirrors amounts / re-bakes. Not a
    // structure change (count unchanged) so onStructureChanged does NOT fire.
    if (cb.onReorder) cb.onReorder(std::min(idx, j), std::max(idx, j));
    if (cb.onChanged) cb.onChanged();
}

void WarpChainEditor::refreshRowVisuals(int idx) {
    if (!chain || idx < 0 || idx >= (int)rows.size() || idx >= (int)chain->size())
        return;
    const WarpOp& op = (*chain)[idx];
    auto& row = rows[idx];
    row.method->setButtonText(methodButtonText(op.method));
    if (const auto* info = warpMethodInfo(op.method))
        row.method->setTooltip(info->tooltip);
    row.enable->setToggleState(op.enabled, juce::dontSendNotification);
    // Disabled op = greyed amount slider so the bypass is visible.
    row.amount->setEnabled(op.enabled);
}

void WarpChainEditor::rebuild() {
    rows.clear();
    if (chain) {
        rows.reserve(chain->size());
        for (int i = 0; i < (int)chain->size(); ++i) {
            Row row;

            row.enable = std::make_unique<juce::ToggleButton>();
            row.enable->setTooltip("Enable / bypass this warp stage.");
            row.enable->onClick = [this, i] {
                if (!chain || i >= (int)chain->size()) return;
                (*chain)[i].enabled = rows[i].enable->getToggleState();
                refreshRowVisuals(i);
                if (cb.onChanged) cb.onChanged();
            };
            addAndMakeVisible(*row.enable);

            row.method = std::make_unique<juce::TextButton>();
            row.method->onClick = [this, i] { showMethodMenu(i); };
            addAndMakeVisible(*row.method);

            row.amount = std::make_unique<juce::Slider>(
                juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
            row.amount->setRange(0.0, 1.0, 0.001);
            row.amount->setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 18);
            row.amount->setTooltip("Morph amount (0 = no effect, 1 = full). "
                                   "Right-click the node param to drive this with "
                                   "an LFO / oscillator.");
            row.amount->setValue((*chain)[i].amount, juce::dontSendNotification);
            row.amount->onValueChange = [this, i] {
                if (!chain || i >= (int)chain->size()) return;
                (*chain)[i].amount = (float)rows[i].amount->getValue();
                if (cb.onChanged) cb.onChanged();
            };
            addAndMakeVisible(*row.amount);

            // Reorder arrows. Shape-bending stages are applied in list order, so
            // the order is part of the sound (fold-then-clip != clip-then-fold).
            const int last = (int)chain->size() - 1;
            row.up = std::make_unique<juce::TextButton>(
                juce::String(juce::CharPointer_UTF8("\xe2\x96\xb2")));  // U+25B2
            row.up->setTooltip(i == 0
                ? "Already the first stage - nothing to move it before."
                : "Move this stage earlier in the chain (it shapes the wave "
                  "sooner). Order matters: fold-then-clip sounds different from "
                  "clip-then-fold.");
            row.up->setEnabled(i > 0);
            row.up->onClick = [this, i] { moveOp(i, -1); };
            addAndMakeVisible(*row.up);

            row.down = std::make_unique<juce::TextButton>(
                juce::String(juce::CharPointer_UTF8("\xe2\x96\xbc")));  // U+25BC
            row.down->setTooltip(i == last
                ? "Already the last stage - nothing to move it after."
                : "Move this stage later in the chain (it shapes the wave "
                  "after the stages above it).");
            row.down->setEnabled(i < last);
            row.down->onClick = [this, i] { moveOp(i, +1); };
            addAndMakeVisible(*row.down);

            row.del = std::make_unique<juce::TextButton>("X");
            row.del->setTooltip("Remove this warp stage.");
            row.del->onClick = [this, i] { removeOp(i); };
            addAndMakeVisible(*row.del);

            rows.push_back(std::move(row));
            refreshRowVisuals(i);
        }
    }
    addBtn.setEnabled(chain != nullptr);
    resized();
    repaint();
}

void WarpChainEditor::showMethodMenu(int idx) {
    if (!chain || idx < 0 || idx >= (int)chain->size()) return;

    // Group methods by domain, preserving registry order within each group.
    std::map<WarpDomain, std::vector<const WarpMethodInfo*>> byDomain;
    std::vector<WarpDomain> domainOrder;
    for (const auto& info : warpMethodRegistry()) {
        if (info.method == WarpMethod::None) continue;
        if (!domainAllowed(info.domain)) continue;  // host restricts the picker
        if (byDomain.find(info.domain) == byDomain.end())
            domainOrder.push_back(info.domain);
        byDomain[info.domain].push_back(&info);
    }

    juce::PopupMenu menu;
    for (WarpDomain d : domainOrder) {
        menu.addSectionHeader(domainLabel(d));
        for (const auto* info : byDomain[d]) {
            const bool ticked = (info->method == (*chain)[idx].method);
            juce::String txt = info->name;
            if (info->recommended)
                txt += "   " + juce::String(juce::CharPointer_UTF8("\xe2\x98\x85"));
            menu.addItem((int)info->method + 1, txt, true, ticked);
        }
    }

    auto* self = this;
    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(rows[idx].method.get()),
        [self, idx](int chosen) {
            if (chosen <= 0) return;
            if (!self->chain || idx >= (int)self->chain->size()) return;
            (*self->chain)[idx].method = (WarpMethod)(chosen - 1);
            self->refreshRowVisuals(idx);
            if (self->cb.onChanged) self->cb.onChanged();
        });
}

void WarpChainEditor::resized() {
    auto a = getLocalBounds().reduced(2);

    auto top = a.removeFromTop(kHeaderH - 2);
    addBtn.setBounds(top.removeFromRight(60));
    header.setBounds(top);

    for (int i = 0; i < (int)rows.size(); ++i) {
        auto r = a.removeFromTop(kRowH).reduced(0, 2);
        rows[i].enable->setBounds(r.removeFromLeft(22));
        rows[i].del->setBounds(r.removeFromRight(22));
        rows[i].down->setBounds(r.removeFromRight(20));
        rows[i].up->setBounds(r.removeFromRight(20));
        r.removeFromRight(4);
        rows[i].amount->setBounds(r.removeFromRight(150));
        r.removeFromRight(4);
        rows[i].method->setBounds(r);
    }
}

void WarpChainEditor::paint(juce::Graphics& g) {
    // Faint separator under the header so the warp section reads as its own
    // group inside whatever host panel embeds it.
    g.setColour(juce::Colours::white.withAlpha(0.10f));
    g.drawHorizontalLine(kHeaderH - 1, 2.0f, (float)getWidth() - 2.0f);

    if (chain && chain->empty()) {
        g.setColour(juce::Colours::white.withAlpha(0.45f));
        g.setFont(juce::FontOptions(12.0f));
        juce::String hint = emptyHint.isNotEmpty()
            ? emptyHint
            : "No shape-bending - press + Add to fold / clip / bend this wave.";
        g.drawText(hint,
                   getLocalBounds().withTop(kHeaderH).reduced(6, 2),
                   juce::Justification::centredLeft, true);
    }
}

} // namespace SoundShop
