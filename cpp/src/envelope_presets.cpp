#include "envelope_presets.h"
#include <set>

namespace SoundShop {

EnvelopePresetManager& EnvelopePresetManager::instance() {
    static EnvelopePresetManager mgr;
    return mgr;
}

EnvelopePresetManager::EnvelopePresetManager() {
    seedBuiltIns();
    // Best-effort load of user presets; missing file is fine.
    auto f = getDefaultFile();
    if (f.existsAsFile()) load(f);
}

// Single source of truth for the factory preset definitions. Both
// seedBuiltIns() (first-run population) and getBuiltInDefaults() (the
// Restore button + value-equality checks) read from this list, so the
// two paths can never drift.
std::vector<EnvelopePreset> EnvelopePresetManager::getBuiltInDefaults() {
    auto makeBuiltIn = [](const char* name, float a, float h, float d,
                          float s, float r, float vs,
                          const char* aExpr, const char* dExpr,
                          const char* rExpr) {
        EnvelopePreset p;
        p.name = name;
        p.builtIn = true;
        // The origin id ties the entry back to its factory definition
        // even after the user renames or edits it. Restore uses this
        // to recognise that a canonical is still represented in the
        // library (under whatever name the user has now) and skip
        // re-adding it.
        p.builtInOriginId = name;
        p.envelope.attackMs = a;
        p.envelope.holdMs   = h;
        p.envelope.decayMs  = d;
        p.envelope.sustain  = s;
        p.envelope.releaseMs = r;
        p.envelope.velocitySensitivity = vs;
        p.envelope.attackCurve.mode = SpectralCurve::Equation;
        p.envelope.attackCurve.expression = aExpr;
        p.envelope.decayCurve.mode = SpectralCurve::Equation;
        p.envelope.decayCurve.expression = dExpr;
        p.envelope.releaseCurve.mode = SpectralCurve::Equation;
        p.envelope.releaseCurve.expression = rExpr;
        return p;
    };

    std::vector<EnvelopePreset> out;
    // "Default" is the manager's baseline - editable copy of the bare
    // AHDSREnvelope defaults. Useful as a one-click "reset" target.
    out.push_back(makeBuiltIn("Default",
        5, 0, 200, 0.7f, 300, 1.0f, "x", "1-x", "1-x"));
    // Plucked / percussive: instant attack, fast decay to zero, no
    // sustain, short release for a string-pick feel.
    out.push_back(makeBuiltIn("Pluck",
        1, 0, 250, 0.0f, 120, 1.0f, "x^0.6", "(1-x)^2", "(1-x)^2"));
    out.push_back(makeBuiltIn("Pluck Long",
        1, 0, 800, 0.0f, 600, 1.0f, "x^0.6", "(1-x)^1.5", "(1-x)^2"));
    // Pad: slow swell in and out, full sustain.
    out.push_back(makeBuiltIn("Pad",
        800, 0, 400, 0.9f, 1200, 0.5f, "x^1.5", "1-x", "1-x^2"));
    // Bass: snappy attack, medium decay to a held body, short release
    // so notes don't smear.
    out.push_back(makeBuiltIn("Bass",
        3, 10, 180, 0.6f, 80, 1.0f, "x^0.5", "(1-x)^1.5", "(1-x)^2"));
    // Organ: pure step-on / step-off, no shaping. Useful as a
    // reference / debugging preset.
    out.push_back(makeBuiltIn("Organ",
        2, 0, 50, 1.0f, 30, 0.0f, "x", "1-x", "1-x"));
    // Strings: medium attack swell, full sustain, long release tail.
    out.push_back(makeBuiltIn("Strings",
        250, 0, 300, 0.85f, 700, 0.7f, "x^1.2", "1-x", "(1-x)^1.5"));
    // Brass: fast attack with a small overshoot effect from the curve,
    // held body, medium release.
    out.push_back(makeBuiltIn("Brass",
        40, 30, 200, 0.8f, 250, 0.9f, "x^0.7", "1-x", "(1-x)^1.2"));
    // Stab: very short hit with no body, useful for percussive
    // stabs / accents.
    out.push_back(makeBuiltIn("Stab",
        2, 20, 80, 0.0f, 40, 1.0f, "x^0.4", "(1-x)^3", "(1-x)^3"));
    return out;
}

void EnvelopePresetManager::seedBuiltIns() {
    presets = getBuiltInDefaults();
}

std::vector<EnvelopePreset> EnvelopePresetManager::getAll() const {
    std::lock_guard<std::mutex> g(mutex);
    return presets;
}

int EnvelopePresetManager::size() const {
    std::lock_guard<std::mutex> g(mutex);
    return (int)presets.size();
}

bool EnvelopePresetManager::getByName(const std::string& name,
                                      EnvelopePreset& out) const {
    std::lock_guard<std::mutex> g(mutex);
    for (auto& p : presets) {
        if (p.name == name) { out = p; return true; }
    }
    return false;
}

std::string EnvelopePresetManager::addOrReplace(const std::string& name,
                                                const AHDSREnvelope& env) {
    {
        std::lock_guard<std::mutex> g(mutex);

        // If an entry with this name exists, overwrite its values and
        // demote it (the saved values are whatever the user typed, so
        // the ★ factory marker drops off). The hidden origin id is
        // PRESERVED across the overwrite so the user's edited "Pluck"
        // is still recognised as descended from canonical "Pluck" -
        // Restore will see it and leave it alone instead of clobbering.
        bool handled = false;
        for (auto& p : presets) {
            if (p.name != name) continue;
            p.envelope = env;
            p.builtIn = false;
            // p.builtInOriginId left unchanged on purpose.
            handled = true;
            break;
        }
        if (!handled) {
            EnvelopePreset np;
            np.name = name;
            np.envelope = env;
            np.builtIn = false;
            // New preset, no factory lineage.
            presets.push_back(np);
        }
    }
    notifyListeners();
    return name;
}

std::string EnvelopePresetManager::addNew(const std::string& desiredName,
                                          const AHDSREnvelope& env) {
    std::string finalName;
    {
        std::lock_guard<std::mutex> g(mutex);
        // Disambiguate by appending " (2)", " (3)", ... until unused.
        // We never overwrite in this path - the caller chose addNew
        // because they want a duplicate, not a replacement.
        finalName = desiredName;
        int n = 2;
        while (true) {
            bool taken = false;
            for (auto& p : presets)
                if (p.name == finalName) { taken = true; break; }
            if (!taken) break;
            finalName = desiredName + " (" + std::to_string(n++) + ")";
        }
        EnvelopePreset np;
        np.name = finalName;
        np.envelope = env;
        np.builtIn = false;
        // builtInOriginId deliberately left empty. A duplicate of a
        // factory preset is a fresh user preset; if we copied the
        // origin id, Restore would treat the duplicate as the
        // canonical's representative and refuse to bring back the
        // real factory entry.
        presets.push_back(np);
    }
    notifyListeners();
    return finalName;
}

bool EnvelopePresetManager::remove(const std::string& name) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> g(mutex);
        for (size_t i = 0; i < presets.size(); ++i) {
            if (presets[i].name == name) {
                // Built-ins are removable too - the user can bring them
                // back with the Restore button. The deletion persists
                // because save() writes the full list, so a deleted
                // built-in stays gone across launches until restored.
                presets.erase(presets.begin() + i);
                removed = true;
                break;
            }
        }
    }
    if (removed) notifyListeners();
    return removed;
}

bool EnvelopePresetManager::rename(const std::string& oldName,
                                   const std::string& newName) {
    bool renamed = false;
    {
        std::lock_guard<std::mutex> g(mutex);
        EnvelopePreset* target = nullptr;
        for (auto& p : presets) if (p.name == oldName) { target = &p; break; }
        if (!target) return false;
        // Disambiguate against existing names (excluding the one being renamed).
        std::string candidate = newName;
        int n = 2;
        while (true) {
            bool dup = false;
            for (auto& q : presets) {
                if (&q == target) continue;
                if (q.name == candidate) { dup = true; break; }
            }
            if (!dup) break;
            candidate = newName + " (" + std::to_string(n++) + ")";
        }
        target->name = candidate;
        // Built-ins lose the visible ★ marker on rename - the renamed
        // entry isn't a "factory preset" any more in the UI sense. The
        // hidden origin id is PRESERVED so Restore can still tell that
        // the canonical is represented (under the new name) and skip
        // re-adding it. To get a fresh canonical back the user has to
        // delete this entry first.
        target->builtIn = false;
        // target->builtInOriginId left unchanged on purpose.
        renamed = true;
    }
    if (renamed) notifyListeners();
    return renamed;
}

int EnvelopePresetManager::restoreBuiltIns() {
    int restored = 0;
    {
        std::lock_guard<std::mutex> g(mutex);
        auto defaults = getBuiltInDefaults();
        for (auto& def : defaults) {
            // Rule 1: if any preset (under any name, with any values)
            // carries this canonical's origin id, the canonical is
            // still represented. Skip - we never replace existing
            // entries. Edited "Pluck" still has originId="Pluck", so
            // restore leaves it alone. Renamed "MyPluck" also still
            // has originId="Pluck", so restore leaves it alone too.
            bool stillRepresented = false;
            for (auto& p : presets) {
                if (p.builtInOriginId == def.name) {
                    stillRepresented = true;
                    break;
                }
            }
            if (stillRepresented) continue;
            // Rule 2: if there's a name collision with an unrelated
            // user preset (the user deleted "Pluck" and then made
            // their own "Pluck" from scratch), don't clobber it.
            // Skip - the user can rename or delete that entry and
            // hit Restore again to bring the canonical back.
            bool nameTaken = false;
            for (auto& p : presets) {
                if (p.name == def.name) { nameTaken = true; break; }
            }
            if (nameTaken) continue;
            // Origin is missing AND name is free - add a fresh
            // canonical copy.
            presets.push_back(def);
            ++restored;
        }
    }
    if (restored > 0) notifyListeners();
    return restored;
}

juce::File EnvelopePresetManager::getDefaultFile() {
    return juce::File::getSpecialLocation(
                juce::File::userApplicationDataDirectory)
            .getChildFile("SoundShop")
            .getChildFile("EnvelopePresets.xml");
}

void EnvelopePresetManager::load(const juce::File& f) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> g(mutex);
        auto root = juce::XmlDocument::parse(f);
        if (!root) {
            // No file (first launch) - seedBuiltIns() already populated
            // the list in the ctor, nothing to do.
            return;
        }
        // The XML is the full library state, including any demoted /
        // deleted built-ins. Replace whatever's in memory with the
        // file contents, then top up with any factory presets that
        // aren't listed at all (so a new SEANCE version that ships
        // additional defaults surfaces them for existing users).
        presets.clear();
        for (auto* el : root->getChildIterator()) {
            if (!el->hasTagName("Preset")) continue;
            auto name = el->getStringAttribute("name").toStdString();
            auto enc  = el->getStringAttribute("data").toStdString();
            if (name.empty() || enc.empty()) continue;
            EnvelopePreset p;
            p.name = name;
            p.builtIn = el->getBoolAttribute("builtIn", false);
            // Origin id ties the entry to its factory lineage. Saved
            // explicitly so it survives rename and edit across launches.
            // Pre-origin-id project files won't have the attribute;
            // fall back to the canonical-name-equals-entry-name guess
            // for entries still marked builtIn, since that's how they
            // would have been seeded before this field existed.
            p.builtInOriginId = el->getStringAttribute(
                "originId",
                p.builtIn ? juce::String(name) : juce::String()).toStdString();
            if (!AHDSREnvelope::decode(enc, p.envelope)) continue;
            presets.push_back(p);
            changed = true;
        }
        // First-run-after-upgrade case: a canonical preset is in the
        // factory list but not in the file. We add it as a fresh
        // built-in. (If the user had deleted it on a previous run, the
        // file would still contain a tombstone via the deleted-list...
        // we don't track tombstones - instead the file lists EVERY
        // entry, so deletion = entry simply not in the file. To prevent
        // accidental re-resurrection, we also persist a separate
        // <DeletedBuiltIns> list below.)
        auto deletedSet = std::set<std::string>{};
        if (auto* del = root->getChildByName("DeletedBuiltIns")) {
            for (auto* d : del->getChildIterator()) {
                if (d->hasTagName("Name"))
                    deletedSet.insert(d->getAllSubText().toStdString());
            }
        }
        auto defaults = getBuiltInDefaults();
        for (auto& def : defaults) {
            if (deletedSet.count(def.name)) continue; // user removed it
            // "Present" = any preset claims this canonical's origin.
            // A renamed entry (different name, same origin) still
            // counts. This matches the Restore button's semantics so
            // the upgrade path and the explicit Restore path behave
            // identically.
            bool present = false;
            for (auto& p : presets)
                if (p.builtInOriginId == def.name) { present = true; break; }
            if (!present) {
                // Also avoid clobbering an unrelated user preset of
                // the same name (the user deleted "Pluck" on an old
                // build, created their own "Pluck" with no origin, and
                // is now loading on a new build that ships "Pluck").
                bool nameTaken = false;
                for (auto& p : presets)
                    if (p.name == def.name) { nameTaken = true; break; }
                if (nameTaken) continue;
                presets.push_back(def);
                changed = true;
            }
        }
    }
    if (changed) notifyListeners();
}

void EnvelopePresetManager::save(const juce::File& f) const {
    juce::XmlElement root("EnvelopePresets");
    {
        std::lock_guard<std::mutex> g(mutex);
        // Persist the full library state. Each entry carries its own
        // builtIn flag and origin id so demoted / renamed built-ins
        // reload with the correct lineage and the Restore button
        // continues to know they're already represented.
        for (auto& p : presets) {
            auto* el = root.createNewChildElement("Preset");
            el->setAttribute("name", juce::String(p.name));
            el->setAttribute("data", juce::String(p.envelope.encode()));
            if (p.builtIn) el->setAttribute("builtIn", true);
            if (!p.builtInOriginId.empty())
                el->setAttribute("originId", juce::String(p.builtInOriginId));
        }
        // Record canonical origin ids that no longer have ANY preset
        // pointing at them - this is the only way the user can really
        // remove a factory preset. Without this, load() would top up
        // missing canonical NAMES and a deleted "Pluck" would silently
        // come back on next launch. We key by origin id (not name) so
        // a renamed-but-not-deleted built-in does NOT count as deleted.
        auto defaults = getBuiltInDefaults();
        juce::XmlElement* del = nullptr;
        for (auto& def : defaults) {
            bool stillRepresented = false;
            for (auto& p : presets) {
                if (p.builtInOriginId == def.name) {
                    stillRepresented = true;
                    break;
                }
            }
            if (!stillRepresented) {
                if (!del) del = root.createNewChildElement("DeletedBuiltIns");
                auto* n = del->createNewChildElement("Name");
                n->addTextElement(juce::String(def.name));
            }
        }
    }
    f.getParentDirectory().createDirectory();
    root.writeTo(f);
}

void EnvelopePresetManager::addListener(std::function<void()> cb) {
    std::lock_guard<std::mutex> g(mutex);
    listeners.push_back(std::move(cb));
}

void EnvelopePresetManager::notifyListeners() {
    std::vector<std::function<void()>> copy;
    {
        std::lock_guard<std::mutex> g(mutex);
        copy = listeners;
    }
    for (auto& cb : copy) if (cb) cb();
}

} // namespace SoundShop
